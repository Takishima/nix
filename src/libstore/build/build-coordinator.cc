#include "nix/store/build/build-coordinator.hh"
#include "nix/store/build/build-registry.hh"
#include "nix/store/store-open.hh"
#include "nix/store/local-fs-store.hh"
#include "nix/store/globals.hh"
#include "nix/util/logging.hh"
#include "nix/util/serialise.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/unix-domain-socket.hh"
#include "nix/util/processes.hh"
#include "nix/util/signals.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <map>
#include <unistd.h>

#ifdef __linux__
#  include <sys/prctl.h>
#endif

namespace nix {

void CoordinatorUnavailable::anchor() {}

/* ------------------------------------------------------------------------ *
 * Control protocol (child ↔ coordinator) — internal, never the client wire.
 * Length-prefixed records; first byte is a tag.
 * ------------------------------------------------------------------------ */

namespace {

// Record tags live in the header (shared with the unit tests).
using namespace coordinator_proto;

/** Read exactly `n` bytes, EINTR-aware (so a daemon interrupt — client HUP —
 *  surfaces via checkInterrupt). Returns false on clean EOF. */
bool readN(int fd, char * buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, buf + got, n - got);
        if (r == 0)
            return false; // EOF
        if (r < 0) {
            if (errno == EINTR) {
                checkInterrupt();
                continue;
            }
            throw SysError("reading from coordinator control socket");
        }
        got += r;
    }
    return true;
}

/** Sanity cap on a single record, so a buggy or hostile peer cannot make
 *  us allocate an arbitrary amount from a 32-bit length prefix. */
constexpr uint32_t maxRecordLen = 256u << 20;

/* Retry budget for the two transient connect races: the lost election
   (a microseconds-wide window between lock and bind) and the idle-exit
   race (EOF before any byte). Deliberately generous. */
constexpr int coordinatorConnectRetries = 50;
constexpr unsigned coordinatorConnectBackoffUs = 100 * 1000;

/** Cap on bytes queued towards one subscriber: past it, a subscriber
 *  that stops reading is dropped like a hangup rather than stalling
 *  every other client. */
constexpr size_t maxConnOutBuf = 64u << 20;

/** Bound on the relay's attach handshake (request write + first byte
 *  back). A healthy coordinator acks within one event-loop iteration;
 *  sustained silence is a wedged-but-listening coordinator, which must
 *  degrade the relay, not block it forever. Env-overridable for tests. */
std::chrono::seconds coordinatorAttachTimeout()
{
    if (auto env = getEnv("NIX_BUILD_COORDINATOR_ATTACH_TIMEOUT"))
        if (auto secs = string2Int<unsigned>(*env))
            return std::chrono::seconds{*secs};
    return std::chrono::seconds{30};
}

std::string coordinatorLockPath(const std::string & socketPath)
{
    return socketPath + ".lock";
}

/** Pop one complete length-prefixed record off the front of `buf`, if a
 *  complete one has arrived. Throws on an oversized length prefix. */
std::optional<std::string> popRecord(std::string & buf)
{
    if (buf.size() < 4)
        return std::nullopt;
    uint32_t len;
    memcpy(&len, buf.data(), 4);
    if (len > maxRecordLen)
        throw Error("coordinator control record too large (%d bytes)", len);
    if (buf.size() < 4 + (size_t) len)
        return std::nullopt;
    std::string rec = buf.substr(4, len);
    buf.erase(0, 4 + (size_t) len);
    return rec;
}

void makeNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        throw SysError("making coordinator fd non-blocking");
}

void makeBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == -1)
        throw SysError("restoring blocking mode on coordinator fd");
}

/** `nix::connect`, but non-blocking: a wedged coordinator's full accept
 *  backlog would park a blocking connect indefinitely; non-blocking it
 *  surfaces as EAGAIN — a failed connect, which callers already handle. */
AutoCloseFD connectNonBlocking(const std::string & socketPath)
{
    AutoCloseFD fd = createUnixDomainSocket();
    makeNonBlocking(fd.get());
    try {
        nix::connect(toSocket(fd.get()), socketPath);
    } catch (SysError & e) {
        if (e.errNo != EINPROGRESS)
            throw;
        /* Unix-domain connects complete synchronously on our platforms,
           but POSIX permits EINPROGRESS: give it a moment to settle. */
        pollfd pfd{.fd = fd.get(), .events = POLLOUT, .revents = 0};
        if (::poll(&pfd, 1, 1000) <= 0)
            throw;
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &soErr, &len) != 0 || soErr != 0)
            throw;
    }
    makeBlocking(fd.get());
    return fd;
}

/** Read one length-prefixed record; nullopt on clean EOF. */
std::optional<std::string> readRecord(int fd)
{
    char lenBuf[4];
    if (!readN(fd, lenBuf, 4))
        return std::nullopt;
    uint32_t len;
    memcpy(&len, lenBuf, 4);
    if (len > maxRecordLen)
        throw Error("coordinator control record too large (%d bytes)", len);
    std::string out(len, '\0');
    if (len && !readN(fd, out.data(), len))
        return std::nullopt;
    return out;
}

void writeRecord(int fd, std::string_view body)
{
    uint32_t len = (uint32_t) body.size();
    std::string framed;
    framed.resize(4);
    memcpy(framed.data(), &len, 4);
    framed.append(body);
    writeFull(fd, framed); // throws on error
}

std::string frameRecord(bool replayed, std::string_view data)
{
    std::string body;
    body.push_back(MSG_FRAME);
    body.push_back(replayed ? 1 : 0);
    body.append(data);
    return body;
}

std::string resultRecord(const BuildResult & res)
{
    std::string body;
    body.push_back(MSG_RESULT);
    body += nlohmann::json(res).dump();
    return body;
}

nlohmann::json activeBuildToJSON(const ActiveBuildStatus & s)
{
    return {
        {"resolvedDrv", s.key.resolvedDrv},
        {"startTime", s.startTime},
        {"subscribers", s.subscriberCount},
        {"logBytes", s.logBytes},
        {"rooted", s.rooted},
    };
}

ActiveBuildStatus activeBuildFromJSON(const nlohmann::json & j)
{
    ActiveBuildStatus s;
    s.key.resolvedDrv = j.at("resolvedDrv").get<std::string>();
    s.startTime = j.at("startTime").get<time_t>();
    s.subscriberCount = j.at("subscribers").get<size_t>();
    s.logBytes = j.at("logBytes").get<uint64_t>();
    s.rooted = j.at("rooted").get<bool>();
    return s;
}

std::string activeRecord(const std::vector<ActiveBuildStatus> & builds)
{
    auto arr = nlohmann::json::array();
    for (const auto & b : builds)
        arr.push_back(activeBuildToJSON(b));
    std::string body;
    body.push_back(MSG_ACTIVE);
    body += arr.dump();
    return body;
}

/* ------------------------------------------------------------------------ *
 * The build child's logger: frames the build's log into the pipe back to the
 * coordinator (the coordinator's fan-out source). Build output reaches us as
 * `resBuildLogLine` results and ordinary `log()` lines (mirrors how the daemon
 * `TunnelLogger` captures a build's output).
 * ------------------------------------------------------------------------ */

class FramingLogger : public Logger
{
    int pipeFd;

    void emit(std::string_view s)
    {
        try {
            writeRecord(pipeFd, frameRecord(false, s));
        } catch (...) {
            // Coordinator gone / pipe closed: nothing we can do from the child.
        }
    }

public:
    explicit FramingLogger(int pipeFd)
        : pipeFd(pipeFd)
    {
    }

    void log(Verbosity, std::string_view s) override
    {
        std::string line(s);
        line.push_back('\n');
        emit(line);
    }

    void logEI(const ErrorInfo & ei) override
    {
        std::ostringstream oss;
        showErrorInfo(oss, ei, false);
        emit(oss.str());
    }

    void result(ActivityId, ResultType type, const Fields & fields) override
    {
        // Newline-terminated so the relay can split coalesced frames back
        // into lines.
        if (type == resBuildLogLine && !fields.empty() && fields[0].type == Logger::Field::tString)
            emit(fields[0].s + "\n");
    }
};

/* ------------------------------------------------------------------------ *
 * The coordinator process
 * ------------------------------------------------------------------------ */

/** A URI that reaches the *same physical store*: `getReference().render()`
 *  drops the location of a `--store /path` local store, so derive an
 *  explicit URI from the store's own directory settings. */
std::string coordinatorStoreUri(Store & store)
{
    if (auto * fs = dynamic_cast<const LocalFSStoreConfig *>(&store.config)) {
        // `root` fully determines state/log/real, so a path round-trips exactly.
        if (auto root = fs->rootDir.get())
            return root->string();
        // No root: pin the physical directories explicitly.
        return fmt(
            "local?store=%s&real=%s&state=%s&log=%s",
            std::string{store.config.storeDir},
            fs->realStoreDir.get().string(),
            fs->stateDir.get().string(),
            fs->logDir.get().string());
    }
    return store.config.getReference().render(/*withParams=*/true);
}

/** The coordinator control socket for `store`: the `NIX_BUILD_COORDINATOR_SOCKET`
 *  override if set, else `$stateDir/coordinator.socket` (one per store). */
std::string coordinatorSocketPath(Store & store)
{
    if (auto env = getEnv("NIX_BUILD_COORDINATOR_SOCKET"); env && !env->empty())
        return *env;
    if (auto * fs = dynamic_cast<const LocalFSStoreConfig *>(&store.config))
        return (fs->stateDir.get() / "coordinator.socket").string();
    // Non-local store: fall back to the state dir from settings.
    return (settings.nixStateDir / "coordinator.socket").string();
}

/** The connecting peer's authenticated identity (its uid via peer-cred);
 *  `nullopt` when it cannot be established. */
std::optional<uid_t> getPeerUid(int fd)
{
#ifdef SO_PEERCRED
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return std::nullopt;
    return cred.uid;
#else
    return geteuid(); // best-effort on non-Linux for this slice
#endif
}

struct Coordinator
{
    std::string socketPath;
    std::string storeUri;
    ref<Store> parseStore; // for parsing drvs/paths (parent never builds)

    /** The registry enforces the single-user gate itself, not only the
     *  transport, so a widened (or buggy) accept check fails closed. */
    SingleIdentityAuthPolicy policy{std::to_string(geteuid())};

    std::unique_ptr<BuildRegistry> registry = makeInMemoryBuildRegistry(policy);

    /** A connected client = one subscription. All socket I/O is
     *  non-blocking — no peer can stall the loop in either direction. */
    struct Conn
    {
        AutoCloseFD fd;
        bool started = false; // sent START_OR_ATTACH yet?
        bool done = false;    // result delivered; close once outBuf drains
        /** Hung up, errored, violated the protocol or overflowed `outBuf`:
         *  unsubscribed (if subscribed) and closed at the end of the loop
         *  iteration. */
        bool dead = false;
        SubscriptionId sub;
        /** The peer's authenticated identity (uid via peer-cred at accept). */
        std::string identity;
        std::string inBuf;
        std::string outBuf;
    };

    /** A running build = the registry MISS that started it. */
    struct Running
    {
        AutoCloseFD pipe; // read end of the build child's frame pipe
        pid_t pid = -1;
        BuildRegistryKey key;
        bool gotResult = false;
        std::string inBuf;
    };

    std::map<int, Conn> conns;      // by control-socket fd
    std::map<int, Running> running; // by build-pipe read fd

    /** Held for the coordinator's lifetime: a live coordinator is exactly
     *  a held lock, so the election winner may safely (re)bind the socket. */
    AutoCloseFD electionLock;

    /** Bound + listening by the election winner *before* this process is
     *  spawned, so a connect can never race the coordinator's startup. */
    AutoCloseFD listenFd;

    Coordinator(std::string socketPath, std::string storeUri, AutoCloseFD electionLock, AutoCloseFD listenFd)
        : socketPath(std::move(socketPath))
        , storeUri(std::move(storeUri))
        , parseStore(openStore(this->storeUri))
        , electionLock(std::move(electionLock))
        , listenFd(std::move(listenFd))
    {
        makeNonBlocking(this->listenFd.get());
    }

    /** Queue a record towards a connection; the poll loop drains it. An
     *  overflowing subscriber is dropped like a hangup. */
    void enqueue(Conn & conn, std::string_view body)
    {
        if (conn.dead)
            return;
        if (conn.outBuf.size() + 4 + body.size() > maxConnOutBuf) {
            conn.dead = true;
            return;
        }
        uint32_t len = (uint32_t) body.size();
        char lenBuf[4];
        memcpy(lenBuf, &len, 4);
        conn.outBuf.append(lenBuf, 4);
        conn.outBuf.append(body);
    }

    /** Write as much of `outBuf` as the socket will take without blocking. */
    void flushConn(Conn & conn)
    {
        while (!conn.outBuf.empty() && !conn.dead) {
            ssize_t n = ::write(conn.fd.get(), conn.outBuf.data(), conn.outBuf.size());
            if (n > 0) {
                conn.outBuf.erase(0, n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;       // wait for POLLOUT
            conn.dead = true; // peer gone (EPIPE etc.)
        }
    }

    /** Fork a build child that runs the derivation and frames its log +
     *  result back over a pipe; a builder crash stays contained. */
    void startBuild(
        const BuildRegistryKey & key, const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode)
    {
        Pipe pipe;
        pipe.create();

        ProcessOptions opts;
        opts.dieWithParent = true; // builders die with the coordinator
        pid_t pid = startProcess(
            [&]() {
                pipe.readSide.close();
                /* Drop every inherited coordinator fd: held here they would
                   outlive a dead coordinator (a hung accept queue, half-open
                   client sockets). The flock survives closing our copy of
                   the lock fd — it lives on the open file description. */
                listenFd.close();
                electionLock.close();
                for (auto & [_, c] : conns)
                    c.fd.close();
                for (auto & [_, r] : running)
                    r.pipe.close();
                int wfd = pipe.writeSide.get();
                try {
                    /* Interrupt state inherited across fork() is poison:
                       stale `ReceiveInterrupts` callbacks pthread_kill
                       threads that did not survive the fork (= abort), the
                       inherited mask blocks the cancellation SIGINT from
                       `onCancel`, and without our own `ReceiveInterrupts`
                       that SIGINT only sets a flag a worker blocked in
                       poll() never looks at. */
                    unix::clearInterruptCallbacks();
                    setInterrupted(false);
                    unix::startSignalHandlerThread();
                    ReceiveInterrupts receiveInterrupts;
#ifdef __linux__
                    prctl(PR_SET_NAME, "nix-coord-build", 0, 0, 0);
#endif
                    // Recursion guard: this build must not relay back.
                    setenv("NIX_BUILD_COORDINATOR_INNER", "1", 1);
                    // Log to the subscribers, not the detached stderr.
                    // (Leaked; the child _exits.)
                    logger = new FramingLogger(wfd);
                    auto store = openStore(storeUri);
                    auto res = store->buildDerivation(drvPath, drv, buildMode);
                    writeRecord(wfd, resultRecord(res));
                } catch (std::exception & e) {
                    BuildResult res;
                    res.inner = BuildResult::Failure{{
                        .status = BuildResult::Failure::MiscFailure,
                        .msg = HintFmt(e.what()),
                    }};
                    try {
                        writeRecord(wfd, resultRecord(res));
                    } catch (...) {
                    }
                }
            },
            opts);

        pipe.writeSide.close();
        Running r;
        r.pid = pid;
        r.key = key;
        int rfd = pipe.readSide.get();
        makeNonBlocking(rfd);
        r.pipe = std::move(pipe.readSide);
        running.emplace(rfd, std::move(r));
    }

    void handleStartOrAttach(int connFd, const std::string & body)
    {
        auto & conn = conns.at(connFd);
        // Body layout: [tag][version][payload]. Check the version first: an
        // incompatible peer is told so (MSG_INCOMPATIBLE) and closed, so it
        // degrades immediately rather than parse-failing deep in the payload
        // and waiting out its attach timeout.
        if (body.size() < 2 || (uint8_t) body[1] != coordProtoVersion) {
            enqueue(conn, std::string(1, MSG_INCOMPATIBLE));
            conn.done = true;
            return;
        }
        StringSource src(std::string_view(body).substr(2)); // skip tag + version
        std::string drvPathStr;
        src >> drvPathStr;
        auto drvPath = parseStore->parseStorePath(drvPathStr);
        BasicDerivation drv;
        readDerivation(src, *parseStore, drv, Derivation::nameFromPath(drvPath));
        uint64_t buildMode, trusted, replayWanted;
        src >> buildMode >> trusted >> replayWanted;
        // `trusted` stays on the wire for layout stability but is
        // client-asserted, so it must never reach an authorization decision.
        (void) trusted;

        // Never trust a client-asserted key: compute it from the derivation
        // bytes we actually *received*, so a forged path cannot attach to a
        // different derivation's build. (The asserted path is still what the
        // build child realises; identical received bytes coalesce either
        // way.)
        Derivation keyDrv;
        static_cast<BasicDerivation &>(keyDrv) = drv;
        BuildRegistryKey key{parseStore->printStorePath(computeStorePath(*parseStore, keyDrv))};

        // The sinks only queue; the poll loop does the writing, so they
        // never block.
        BuildLogSink liveSink = [this, connFd](const BuildLogFrame & f) {
            if (auto it = conns.find(connFd); it != conns.end())
                enqueue(it->second, frameRecord(f.replayed, f.data));
        };
        BuildResultSink resultSink = [this, connFd](const BuildResult & r) {
            if (auto it = conns.find(connFd); it != conns.end()) {
                enqueue(it->second, resultRecord(r));
                it->second.done = true;
            }
        };

        // Authorize the *peer's* authenticated identity, not our own;
        // `trusted` is deliberately not taken from the wire (see above).
        BuildAuth auth{.identity = conn.identity, .trusted = false};
        SubscribeOptions opts;
        opts.replayWanted = replayWanted != 0;

        auto attach = registry->startOrAttach(auth, key, liveSink, resultSink, opts, [this, key] { onCancel(key); });
        if (!attach) {
            // Denied (uniform — no existence oracle). Close without revealing.
            conn.done = true;
            return;
        }
        conn.sub = attach->id;
        conn.started = true;

        /* Ack before forking the build: the relay bounds its wait for our
           first byte, and a quiet MISS is otherwise silent until the
           build's first log line — indistinguishable from a wedged
           coordinator. */
        enqueue(conn, std::string(1, MSG_ATTACHED));

        if (attach->started)
            startBuild(key, drvPath, drv, (BuildMode) buildMode);
    }

    void handleQueryActive(int connFd)
    {
        // The registry filters to builds this identity may observe.
        auto & conn = conns.at(connFd);
        BuildAuth auth{.identity = conn.identity, .trusted = false};
        enqueue(conn, activeRecord(registry->queryActive(auth)));
    }

    /** Registry asked us to cancel `key`'s build (refcount 0, no root). */
    void onCancel(const BuildRegistryKey & key)
    {
        for (auto & [fd, r] : running)
            if (r.key == key && r.pid > 0) {
                ::kill(r.pid, SIGINT);
                break;
            }
    }

    void handleBuildRecord(Running & r, const std::string & rec)
    {
        if (rec.empty())
            return;
        char tag = rec[0];
        if (tag == MSG_FRAME) {
            registry->log(r.key, std::string_view(rec).substr(2));
        } else if (tag == MSG_RESULT) {
            auto res = nlohmann::json::parse(rec.substr(1)).get<BuildResult>();
            r.gotResult = true;
            registry->finish(r.key, res);
        }
    }

    /** Drain whatever the build child's pipe has, without blocking; complete
     *  records are dispatched, a partial one waits in `inBuf`. */
    void onBuildPipeReadable(int rfd)
    {
        auto & r = running.at(rfd);
        char buf[65536];
        while (true) {
            ssize_t n = ::read(rfd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                throw SysError("reading from build child pipe");
            }
            if (n == 0) { // pipe EOF: the child is gone
                if (!r.gotResult) {
                    BuildResult res;
                    res.inner = BuildResult::Failure{{
                        .status = BuildResult::Failure::MiscFailure,
                        .msg = HintFmt("build child exited without a result"),
                    }};
                    registry->finish(r.key, res);
                }
                reap(r.pid);
                running.erase(rfd);
                return;
            }
            r.inBuf.append(buf, n);
            while (auto rec = popRecord(r.inBuf))
                handleBuildRecord(r, *rec);
        }
    }

    void dispatchConnRecord(int connFd, const std::string & rec)
    {
        auto & conn = conns.at(connFd);
        if (rec.empty()) {
            conn.dead = true;
            return;
        }
        char tag = rec[0];
        if (tag == MSG_QUERY_ACTIVE) {
            // A query connection never becomes a subscriber, so it takes no
            // refcount: answer and close.
            handleQueryActive(connFd);
            conn.done = true;
            return;
        }
        if (tag != MSG_START_OR_ATTACH) {
            conn.dead = true;
            return;
        }
        try {
            handleStartOrAttach(connFd, rec);
        } catch (std::exception & e) {
            printError("coordinator: bad START_OR_ATTACH: %s", e.what());
            conn.dead = true;
        }
    }

    /** Drain the client socket without blocking. After the one request
     *  record, the only meaningful event is EOF = refcounted unsubscribe. */
    void onConnReadable(int connFd)
    {
        auto & conn = conns.at(connFd);
        char buf[65536];
        while (!conn.dead) {
            ssize_t n = ::read(connFd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return;
                conn.dead = true;
                return;
            }
            if (n == 0) {
                conn.dead = true;
                return;
            }
            if (conn.started || conn.done)
                continue; // post-request noise
            conn.inBuf.append(buf, n);
            while (!conn.started && !conn.done && !conn.dead) {
                auto rec = popRecord(conn.inBuf);
                if (!rec)
                    break;
                dispatchConnRecord(connFd, *rec);
            }
        }
    }

    void reap(pid_t pid)
    {
        if (pid > 0) {
            int status;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }

    [[noreturn]] void run()
    {
        /* The lock file doubles as the holder's pid file — `ps` only shows
           the argv inherited from the spawner. Best-effort: the lock works
           regardless. */
        if (ftruncate(electionLock.get(), 0) == 0) {
            auto pid = fmt("%d\n", getpid());
            [[maybe_unused]] auto _ = pwrite(electionLock.get(), pid.data(), pid.size(), 0);
        }

        /* SIGTERM/SIGINT (systemd stop, an operator) must wake the poll
           below out of its sleep so the cleanup at the bottom runs;
           otherwise the only exit is SIGKILL, which leaks the socket and
           lock files. */
        ReceiveInterrupts receiveInterrupts;

        int idleTicks = 0;
        while (true) {
            std::vector<pollfd> fds;
            fds.push_back({listenFd.get(), POLLIN, 0});
            for (auto & [fd, c] : conns)
                fds.push_back({fd, (short) (POLLIN | (c.outBuf.empty() ? 0 : POLLOUT)), 0});
            for (auto & [fd, _] : running)
                fds.push_back({fd, POLLIN, 0});

            int n = ::poll(fds.data(), fds.size(), 10000);
            if (getInterrupted())
                break; // asked to terminate: clean up below, like idle-exit
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                throw SysError("coordinator poll");
            }

            if (n == 0) { // idle tick
                if (conns.empty() && running.empty() && ++idleTicks >= 2)
                    break; // idle-exit; the next build lazily respawns
                continue;
            }
            idleTicks = 0;

            // Accept first so new work is picked up promptly.
            if (fds[0].revents & POLLIN) {
                while (true) {
                    int c = ::accept(listenFd.get(), nullptr, nullptr);
                    if (c < 0)
                        break;
                    // Same-uid transport gate (defense in depth: the
                    // single-identity registry policy re-checks the identity).
                    if (auto uid = getPeerUid(c); uid && *uid == geteuid()) {
                        makeNonBlocking(c);
                        Conn conn;
                        conn.fd = AutoCloseFD{c};
                        conn.identity = std::to_string(*uid);
                        conns.emplace(c, std::move(conn));
                    } else
                        ::close(c);
                }
            }

            /* Snapshot fds: handlers mutate `running`. A malformed record
               must only take down the connection or build it arrived on,
               never the coordinator — that would kill every other
               in-flight build. */
            std::vector<std::pair<int, short>> events;
            for (size_t i = 1; i < fds.size(); ++i)
                if (fds[i].revents)
                    events.emplace_back(fds[i].fd, fds[i].revents);

            for (auto & [fd, revents] : events) {
                if (auto it = conns.find(fd); it != conns.end()) {
                    if (revents & POLLOUT)
                        flushConn(it->second);
                    if (revents & (POLLIN | POLLHUP | POLLERR))
                        try {
                            onConnReadable(fd);
                        } catch (std::exception & e) {
                            printError("coordinator: dropping client connection: %s", e.what());
                            it->second.dead = true;
                        }
                } else if (running.count(fd) && (revents & (POLLIN | POLLHUP | POLLERR))) {
                    try {
                        onBuildPipeReadable(fd);
                    } catch (std::exception & e) {
                        printError("coordinator: bad record from build child: %s", e.what());
                        auto rit = running.find(fd);
                        if (rit != running.end()) {
                            if (!rit->second.gotResult) {
                                BuildResult res;
                                res.inner = BuildResult::Failure{{
                                    .status = BuildResult::Failure::MiscFailure,
                                    .msg = HintFmt("build child sent a malformed record"),
                                }};
                                registry->finish(rit->second.key, res);
                            }
                            if (rit->second.pid > 0)
                                ::kill(rit->second.pid, SIGKILL);
                            reap(rit->second.pid);
                            running.erase(rit);
                        }
                    }
                }
            }

            /* Flush freshly queued output, then sweep: a dead connection is
               unsubscribed (refcounted, like a hangup) and closed; a done one
               closes once its queue has drained. */
            for (auto it = conns.begin(); it != conns.end();) {
                auto & c = it->second;
                if (!c.outBuf.empty() && !c.dead)
                    flushConn(c);
                if (c.dead || (c.done && c.outBuf.empty())) {
                    if (c.started && !c.done)
                        registry->unsubscribe(c.sub, DetachReason::Hup);
                    it = conns.erase(it);
                } else
                    ++it;
            }
        }

        /* Leave nothing behind that could mislead the next election:
           remove the socket first (a fast-path connect must not find a
           dead socket), then the lock file — whose disappearance is safe
           because `electCoordinator` re-checks the inode it locked
           against the path. In-flight build children die with us
           (PDEATHSIG); their subscribers see EOF and degrade. */
        unlink(socketPath.c_str());
        unlink(coordinatorLockPath(socketPath).c_str());
        _exit(0);
    }
};

/** The election: an exclusive lock on `${socketPath}.lock`. A closed fd
 *  means another process is (becoming) the coordinator; an unusable lock
 *  file throws `CoordinatorUnavailable` — it must not look like a lost
 *  election, and the relay callers degrade to an uncoordinated build
 *  instead of failing the build. */
AutoCloseFD electCoordinator(const std::string & socketPath)
{
    auto lockPath = coordinatorLockPath(socketPath);
    /* A coordinator unlinks its lock file on exit, so winning the flock
       is not enough: the inode we opened may have been detached (or
       replaced) between our open and our flock, and a lock on a detached
       inode excludes nobody. Re-check identity after locking; the loop
       can only iterate while a coordinator is exiting concurrently. */
    for (int attempt = 0; attempt < 10; ++attempt) {
        AutoCloseFD lock{open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)};
        if (!lock) {
            auto err = errno;
            throw CoordinatorUnavailable("opening the coordinator election lock '%s': %s", lockPath, strerror(err));
        }
        if (flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK)
                return {}; // someone else is (starting to be) the coordinator
            auto err = errno;
            throw CoordinatorUnavailable("locking the coordinator election lock '%s': %s", lockPath, strerror(err));
        }
        struct stat fdSt, pathSt;
        if (fstat(lock.get(), &fdSt) == 0 && ::stat(lockPath.c_str(), &pathSt) == 0 && fdSt.st_ino == pathSt.st_ino
            && fdSt.st_dev == pathSt.st_dev)
            return lock;
        // We locked a corpse; run again on the file currently at the path.
    }
    throw CoordinatorUnavailable("the coordinator election at '%s' would not settle", lockPath);
}

/** Spawn the coordinator for an election already won: it inherits (and
 *  from then on owns) the held lock and the already-listening socket. */
void spawnCoordinator(
    const std::string & socketPath, const std::string & storeUri, AutoCloseFD electionLock, AutoCloseFD listenFd)
{
    /* The coordinator process itself: detach, re-arm signals, run the loop.
       Runs only in the grandchild (moves the inherited fds into place there). */
    auto coordinatorMain = [&]() {
        ::setsid();
        /* Detach from the spawner's stdio and logger: the coordinator
           outlives its spawner, and one write(2) into an inherited
           pipe nobody reads any more would park the single-threaded
           loop forever — a listening-but-never-accepting socket every
           later relay hangs on. */
        if (AutoCloseFD devNull{open("/dev/null", O_RDWR)}; devNull) {
            dup2(devNull.get(), STDIN_FILENO);
            dup2(devNull.get(), STDOUT_FILENO);
            dup2(devNull.get(), STDERR_FILENO);
        }
        logger = makeSimpleLogger(false).release(); // leaked; this process _exits
#ifdef __linux__
        // The inherited argv is the spawner's; let `ps`/`pgrep` find us.
        prctl(PR_SET_NAME, "nix-coordinator", 0, 0, 0);
#endif
        /* Inherited interrupt state is the spawner's: stale callbacks
           pthread_kill threads that did not survive the fork, and a
           pending flag would kill us on the first `checkInterrupt`. */
        unix::clearInterruptCallbacks();
        setInterrupted(false);
        /* Re-arm signal handling — the inherited mask blocks SIGTERM
           and the sigwait thread did not survive the fork. Without
           this the coordinator can only be SIGKILLed, which leaks the
           socket and lock files. */
        unix::startSignalHandlerThread();
        Coordinator coord{socketPath, storeUri, std::move(electionLock), std::move(listenFd)};
        coord.run(); // [[noreturn]]
    };

    /* Double-fork: the intermediate child spawns the detached coordinator and
       exits at once, so the coordinator reparents to init and is reaped by it
       on idle-exit. A single fork would leave an idle-exited coordinator a
       zombie under a long-lived spawner whose SIGCHLD is SIG_DFL (they would
       accrue across re-elections). We reap the short-lived intermediate here. */
    ProcessOptions coordOpts;
    coordOpts.dieWithParent = false; // reparents to init; must not die with the intermediate

    ProcessOptions intermediateOpts;
    intermediateOpts.dieWithParent = false; // outlives the spawning connection

    Pid intermediate{startProcess(
        [&]() {
            startProcess(coordinatorMain, coordOpts);
            _exit(0);
        },
        intermediateOpts)};
    intermediate.wait();
}

/** One non-blocking attempt to reach the coordinator, becoming its host
 *  if there is none. On an election win, bind + listen *here* — before
 *  spawning — so the subsequent connect cannot race the coordinator's
 *  startup. A closed fd only in the narrow lost-election race. */
AutoCloseFD connectToCoordinator(const std::string & socketPath, const std::string & storeUri)
{
    // Fast path: a coordinator is already serving. (Non-blocking: a full
    // accept backlog — a wedged coordinator — must fail, not park us.)
    try {
        return connectNonBlocking(socketPath);
    } catch (SysError &) {
    }

    // Nothing serving: run the election ourselves.
    if (auto lock = electCoordinator(socketPath)) {
        // We won: any existing socket file is stale (a live coordinator
        // would hold the lock), so bind over it.
        AutoCloseFD listenFd;
        try {
            listenFd = createUnixDomainSocket(socketPath, 0600);
        } catch (SysError & e) {
            /* We hold the lock but cannot bind in this directory: the
               location is unusable for this process, not transiently
               contended (the lock is released by unwinding). */
            throw CoordinatorUnavailable("%s", e.message());
        }
        spawnCoordinator(socketPath, storeUri, std::move(lock), std::move(listenFd));
        return connectNonBlocking(socketPath);
    }

    // Lost the election; the winner binds before it spawns, so the
    // unconnectable window is tiny: try once more.
    try {
        return connectNonBlocking(socketPath);
    } catch (SysError &) {
        return {};
    }
}

} // namespace

struct CoordinatorRelayPump::Impl
{
    /* Emit `resBuildLogLine` results, not `log()`: the stderr tunnels
       forward results unconditionally, while `log()` is dropped at the
       pinned low verbosity of e.g. `nix-store --serve`. */
    Activity act;

    /** Partially received line (frames may split lines, lines may span frames). */
    std::string pendingLine;

    /** Partially received length-prefixed record. */
    std::string pendingRecord;

    /** Whether `feed` has seen any bytes at all. */
    bool received = false;

    Impl(Logger & logger, const std::string & drvPathStr)
        : act(logger, lvlInfo, actBuild, fmt("building '%s'", drvPathStr), Logger::Fields{drvPathStr, "", 1, 1})
    {
    }

    void emitLines(std::string_view data)
    {
        pendingLine.append(data);
        size_t pos;
        while ((pos = pendingLine.find('\n')) != std::string::npos) {
            act.result(resBuildLogLine, pendingLine.substr(0, pos));
            pendingLine.erase(0, pos + 1);
        }
    }

    std::optional<BuildResult> feedRecord(std::string_view body)
    {
        if (body.empty())
            return std::nullopt;
        char tag = body[0];
        if (tag == MSG_FRAME) {
            if (body.size() > 2)
                emitLines(body.substr(2));
        } else if (tag == MSG_RESULT) {
            if (!pendingLine.empty())
                act.result(resBuildLogLine, pendingLine);
            return nlohmann::json::parse(body.substr(1)).get<BuildResult>();
        } else if (tag == MSG_INCOMPATIBLE) {
            // The coordinator speaks a different control-protocol version;
            // degrade to an uncoordinated build now, do not wait for a result
            // that will never come.
            throw CoordinatorUnavailable("the build coordinator speaks an incompatible control-protocol version");
        }
        return std::nullopt;
    }

    std::optional<BuildResult> feed(std::string_view data)
    {
        if (!data.empty())
            received = true;
        pendingRecord.append(data);
        while (true) {
            if (pendingRecord.size() < 4)
                return std::nullopt;
            uint32_t len;
            memcpy(&len, pendingRecord.data(), 4);
            if (len > maxRecordLen)
                throw Error("coordinator control record too large (%d bytes)", len);
            if (pendingRecord.size() < 4 + (size_t) len)
                return std::nullopt;
            auto res = feedRecord(std::string_view(pendingRecord).substr(4, len));
            pendingRecord.erase(0, 4 + (size_t) len);
            if (res)
                return res;
        }
    }
};

CoordinatorRelayPump::CoordinatorRelayPump(Logger & logger, const std::string & drvPathStr)
    : impl(std::make_unique<Impl>(logger, drvPathStr))
{
}

CoordinatorRelayPump::~CoordinatorRelayPump() = default;
CoordinatorRelayPump::CoordinatorRelayPump(CoordinatorRelayPump &&) noexcept = default;
CoordinatorRelayPump & CoordinatorRelayPump::operator=(CoordinatorRelayPump &&) noexcept = default;

std::optional<BuildResult> CoordinatorRelayPump::feed(std::string_view data)
{
    return impl->feed(data);
}

bool CoordinatorRelayPump::receivedAnything() const
{
    return impl->received;
}

std::optional<CoordinatorRelaySession> tryStartCoordinatorRelay(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted)
{
    auto socketPath = coordinatorSocketPath(store);
    auto storeUri = coordinatorStoreUri(store);
    auto fd = connectToCoordinator(socketPath, storeUri);
    if (!fd)
        return std::nullopt; // lost-election race; the caller schedules a retry

    auto timeout = coordinatorAttachTimeout();

    // START_OR_ATTACH (keyed on the resolved drv — a BasicDerivation).
    std::string request;
    {
        StringSink p;
        p << store.printStorePath(drvPath);
        writeDerivation(p, store, drv);
        p << (uint64_t) buildMode << (uint64_t) (trusted ? 1 : 0) << (uint64_t) 1 /*replayWanted*/;
        request.push_back(MSG_START_OR_ATTACH);
        request.push_back((char) coordProtoVersion); // version byte follows the tag
        request += p.s;
    }

    for (bool stole = false;; stole = true) {
        bool silent = false;

        /* Bound the request write: a wedged coordinator stops draining
           its sockets, and an unbounded write would hang this (possibly
           daemon-session) process forever. */
        struct timeval tv{.tv_sec = (time_t) timeout.count(), .tv_usec = 0};
        setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        try {
            writeRecord(fd.get(), request);
        } catch (SysError & e) {
            if (e.errNo != EAGAIN && e.errNo != EWOULDBLOCK)
                // Peer hung up mid-handshake: the idle-exit race, retried
                // by the caller exactly like EOF-before-bytes.
                return std::nullopt;
            silent = true;
        }

        /* Bounded wait for the first byte back (the ATTACHED ack). Data
           and EOF are both fine — the caller's pump handles them; only
           sustained silence is fatal, because that is what a
           wedged-but-listening coordinator looks like. */
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!silent) {
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                silent = true;
                break;
            }
            pollfd pfd{.fd = fd.get(), .events = POLLIN, .revents = 0};
            int n = ::poll(&pfd, 1, (int) remaining.count());
            if (n > 0)
                break;
            if (n < 0 && errno != EINTR)
                throw SysError("waiting for the build coordinator's acknowledgement");
            checkInterrupt();
        }

        if (!silent)
            return CoordinatorRelaySession{
                .socket = std::move(fd),
                .pump = CoordinatorRelayPump(logger, store.printStorePath(drvPath)),
            };

        if (stole)
            // Our own fresh coordinator is silent too: something is
            // deeply wrong; degrade rather than loop.
            throw CoordinatorUnavailable(
                "the build coordinator at '%s' did not acknowledge within %d seconds",
                socketPath,
                (uint64_t) timeout.count());

        /* A listener took our connection but never answered. A live
           coordinator is exactly a held election lock — so if the lock is
           winnable, that listener is an ownerless leftover (e.g. of a
           process killed mid-teardown) squatting the path: the fast-path
           connect keeps succeeding against it, the election would never
           run again, and every future build would pay this timeout and
           degrade. Steal the path: win the election, bind over it, serve
           a fresh coordinator, and redo the handshake once. A *held*
           lock is a live-but-unresponsive holder nothing can steal —
           degrade. */
        auto lock = electCoordinator(socketPath);
        if (!lock)
            throw CoordinatorUnavailable(
                "the build coordinator at '%s' did not acknowledge within %d seconds",
                socketPath,
                (uint64_t) timeout.count());
        try {
            auto listenFd = createUnixDomainSocket(socketPath, 0600);
            spawnCoordinator(socketPath, storeUri, std::move(lock), std::move(listenFd));
            fd = connectNonBlocking(socketPath);
        } catch (SysError & e) {
            throw CoordinatorUnavailable("%s", e.message());
        }
    }
}

CoordinatorRelaySession startCoordinatorRelay(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted)
{
    for (int attempt = 0; attempt < coordinatorConnectRetries; ++attempt) {
        checkInterrupt();
        if (auto session = tryStartCoordinatorRelay(store, drvPath, drv, buildMode, logger, trusted))
            return std::move(*session);
        usleep(coordinatorConnectBackoffUs);
    }
    /* Persistently unreachable but not provably impossible — e.g. an
       orphaned process still holds the election lock. Same degradation
       as an unusable socket location: dedup is lost, the build is not. */
    throw CoordinatorUnavailable("could not reach the build coordinator at '%s'", coordinatorSocketPath(store));
}

BuildResult relayBuildToCoordinator(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted)
{
    for (int attempt = 0;; ++attempt) {
        auto session = startCoordinatorRelay(store, drvPath, drv, buildMode, logger, trusted);

        // Relay frames to the ambient logger (→ the client) until the result.
        char buf[8192];
        while (true) {
            ssize_t r = ::read(session.socket.get(), buf, sizeof(buf));
            if (r == 0) {
                if (!session.pump.receivedAnything() && attempt < coordinatorConnectRetries) {
                    // The coordinator idle-exited between our connect and our
                    // request; the next attempt re-runs the election.
                    break;
                }
                /* The coordinator went away mid-build (crash, shutdown).
                   Degrading is sound: the mode is bmNormal and the
                   caller re-checks output validity either way. */
                throw CoordinatorUnavailable("the build coordinator closed the connection without a result");
            }
            if (r < 0) {
                if (errno == EINTR) {
                    checkInterrupt();
                    continue;
                }
                /* A read error mid-stream (e.g. ECONNRESET) is the coordinator
                   going away, exactly like a mid-stream EOF: degrade to an
                   uncoordinated build instead of failing it. */
                auto err = errno;
                throw CoordinatorUnavailable("the build coordinator connection failed: %s", strerror(err));
            }
            if (auto res = session.pump.feed(std::string_view(buf, r)))
                return *res;
        }
    }
}

BuildResult processCoordinatorRelayRecords(
    const std::function<std::optional<std::string>()> & readRecord, Logger & logger, const std::string & drvPathStr)
{
    CoordinatorRelayPump pump(logger, drvPathStr);
    while (auto rec = readRecord()) {
        // Re-frame with the length prefix, so this entry point exercises
        // the same incremental decoder the event-loop path uses.
        uint32_t len = (uint32_t) rec->size();
        std::string framed;
        framed.resize(4);
        memcpy(framed.data(), &len, 4);
        framed.append(*rec);
        if (auto res = pump.feed(framed))
            return *res;
    }
    throw Error("build coordinator closed the connection without a result");
}

std::vector<ActiveBuildStatus> queryActiveBuildsViaCoordinator(Store & store)
{
    auto socketPath = coordinatorSocketPath(store);

    // Never spawn a coordinator for a query: nothing listening means
    // nothing in flight.
    AutoCloseFD fd;
    try {
        fd = nix::connect(socketPath);
    } catch (SysError &) {
        return {};
    }

    {
        std::string body;
        body.push_back(MSG_QUERY_ACTIVE);
        writeRecord(fd.get(), body);
    }

    auto rec = readRecord(fd.get());
    if (!rec || rec->empty() || (*rec)[0] != MSG_ACTIVE)
        throw Error("build coordinator at '%s' gave no active-builds answer", socketPath);

    std::vector<ActiveBuildStatus> out;
    for (const auto & j : nlohmann::json::parse(rec->substr(1)))
        out.push_back(activeBuildFromJSON(j));
    return out;
}

} // namespace nix
