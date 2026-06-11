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

#include <poll.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <csignal>
#include <cerrno>
#include <map>
#include <unistd.h>

namespace nix {

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

/** A sanity cap on a single control/pipe record, so a buggy or hostile peer
 *  cannot make us allocate an arbitrary amount from a 32-bit length prefix. A
 *  START_OR_ATTACH carries one resolved derivation; log frames are line-sized.
 *  256 MiB is far above any legitimate record. */
constexpr uint32_t maxRecordLen = 256u << 20;

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

/** A URI the coordinator can `openStore()` to reach the *same physical store*
 *  the daemon serves. `getReference().render()` drops the location of a
 *  `--store /path` local store (it renders bare `local`), so derive an explicit,
 *  round-trippable URI from the store's own directory settings. */
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

    /** The registry enforces the single-user gate itself (only the
     *  coordinator's own identity may build/attach/observe), not only the
     *  transport: if the same-uid `accept` check is ever widened ahead of a
     *  real multi-tenant policy, the registry fails closed. */
    SingleIdentityAuthPolicy policy{std::to_string(geteuid())};

    std::unique_ptr<BuildRegistry> registry = makeInMemoryBuildRegistry(policy);

    /** A connected daemon child = one subscription. */
    struct Conn
    {
        AutoCloseFD fd;
        bool started = false; // sent START_OR_ATTACH yet?
        bool done = false;    // result delivered; close after flush
        SubscriptionId sub;
        BuildRegistryKey key;
        /** The peer's authenticated identity (uid via peer-cred at accept). */
        std::string identity;
    };

    /** A running build = the registry MISS that started it. */
    struct Running
    {
        AutoCloseFD pipe; // read end of the build child's frame pipe
        pid_t pid = -1;
        BuildRegistryKey key;
        bool gotResult = false;
    };

    std::map<int, Conn> conns;      // by control-socket fd
    std::map<int, Running> running; // by build-pipe read fd

    /** The held election lock (`${socketPath}.lock`): exclusively flocked for
     *  the coordinator's lifetime, so a live coordinator is exactly a held
     *  lock and the winner of `electCoordinator` may safely (re)bind the
     *  socket. */
    AutoCloseFD electionLock;

    /** The listening control socket. Bound + listening by the election
     *  winner *before* this process is spawned, so a client's connect can
     *  never race the coordinator's startup. */
    AutoCloseFD listenFd;

    Coordinator(std::string socketPath, std::string storeUri, AutoCloseFD electionLock, AutoCloseFD listenFd)
        : socketPath(std::move(socketPath))
        , storeUri(std::move(storeUri))
        , parseStore(openStore(this->storeUri))
        , electionLock(std::move(electionLock))
        , listenFd(std::move(listenFd))
    {
    }

    /** Fork a build child that runs the resolved derivation and frames its log
     *  + result back over a pipe. Builders stay forked, so a builder crash is
     *  contained exactly as today. */
    void startBuild(const BuildRegistryKey & key, const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode)
    {
        Pipe pipe;
        pipe.create();

        ProcessOptions opts;
        opts.dieWithParent = true; // builders die with the coordinator
        pid_t pid = startProcess(
            [&]() {
                pipe.readSide.close();
                int wfd = pipe.writeSide.get();
                try {
                    // The inherited mask blocks SIGINT (handled by a thread
                    // that doesn't survive fork); without re-arming, the
                    // cancellation SIGINT from `onCancel` is never delivered.
                    unix::startSignalHandlerThread();
                    // Recursion guard: this build must run locally, not relay
                    // back to the coordinator (the goal checks this env var).
                    setenv("NIX_BUILD_COORDINATOR_INNER", "1", 1);
                    auto store = openStore(storeUri);
                    logger = new FramingLogger(wfd); // child's ambient logger (leaked; child _exits)
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
        r.pipe = std::move(pipe.readSide);
        running.emplace(rfd, std::move(r));
    }

    void handleStartOrAttach(int connFd, const std::string & body)
    {
        auto & conn = conns.at(connFd);
        StringSource src(std::string_view(body).substr(1)); // skip tag
        std::string drvPathStr;
        src >> drvPathStr;
        auto drvPath = parseStore->parseStorePath(drvPathStr);
        BasicDerivation drv;
        readDerivation(src, *parseStore, drv, Derivation::nameFromPath(drvPath));
        uint64_t buildMode, trusted, replayWanted;
        src >> buildMode >> trusted >> replayWanted;

        // Never trust a client-asserted key: compute it from the derivation
        // we actually *received* (`computeStorePath` derives the canonical
        // store path of those bytes without writing them), so a forged path
        // in START_OR_ATTACH cannot collide with — or attach to — the build
        // of a different derivation. The asserted path is still the one the
        // build child realises: for an input-addressed derivation it need not
        // equal the canonical path of its `BasicDerivation` projection (the
        // `inputDrvs` are not part of what is sent), and identical received
        // bytes still coalesce either way.
        Derivation keyDrv;
        static_cast<BasicDerivation &>(keyDrv) = drv;
        BuildRegistryKey key{parseStore->printStorePath(computeStorePath(*parseStore, keyDrv))};
        conn.key = key;

        // Fan-out sinks write straight down this child's control socket; the
        // child relays the bytes verbatim to its client (public wire unchanged).
        BuildLogSink liveSink = [this, connFd](const BuildLogFrame & f) {
            try {
                writeRecord(connFd, frameRecord(f.replayed, f.data));
            } catch (...) {
            }
        };
        BuildResultSink resultSink = [this, connFd](const BuildResult & r) {
            try {
                writeRecord(connFd, resultRecord(r));
            } catch (...) {
            }
            if (auto it = conns.find(connFd); it != conns.end())
                it->second.done = true;
        };

        // Authorize the *peer's* authenticated identity (peer-cred at accept),
        // not our own: under the single-identity policy a foreign peer is
        // denied uniformly even if the accept gate let it through.
        BuildAuth auth{.identity = conn.identity, .trusted = trusted != 0};
        SubscribeOptions opts;
        opts.replayWanted = replayWanted != 0;

        auto attach = registry->startOrAttach(
            auth, key, liveSink, resultSink, opts, [this, key] { onCancel(key); });
        if (!attach) {
            // Denied (uniform — no existence oracle). Close without revealing.
            conn.done = true;
            return;
        }
        conn.sub = attach->id;
        conn.started = true;

        if (attach->started)
            startBuild(key, drvPath, drv, (BuildMode) buildMode);
    }

    void handleQueryActive(int connFd)
    {
        // peer-cred at accept established the caller's identity; the registry
        // filters to builds it may observe (under the single-identity policy,
        // only the coordinator's own identity observes anything).
        BuildAuth auth{.identity = conns.at(connFd).identity, .trusted = true};
        try {
            writeRecord(connFd, activeRecord(registry->queryActive(auth)));
        } catch (...) {
        }
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

    void onBuildPipeReadable(int rfd)
    {
        auto rec = readRecord(rfd);
        auto it = running.find(rfd);
        if (it == running.end())
            return;
        if (!rec) { // pipe EOF
            if (!it->second.gotResult) {
                BuildResult res;
                res.inner = BuildResult::Failure{{
                    .status = BuildResult::Failure::MiscFailure,
                    .msg = HintFmt("build child exited without a result"),
                }};
                registry->finish(it->second.key, res);
            }
            reap(it->second.pid);
            running.erase(it);
            return;
        }
        if (rec->empty())
            return;
        char tag = (*rec)[0];
        if (tag == MSG_FRAME) {
            bool replayed = rec->size() > 1 && (*rec)[1] != 0;
            (void) replayed; // live build frames are not replayed
            registry->log(it->second.key, std::string_view(*rec).substr(2));
        } else if (tag == MSG_RESULT) {
            auto res = nlohmann::json::parse(rec->substr(1)).get<BuildResult>();
            it->second.gotResult = true;
            registry->finish(it->second.key, res);
        }
    }

    void onConnReadable(int connFd)
    {
        auto & conn = conns.at(connFd);
        if (!conn.started) {
            auto rec = readRecord(connFd);
            if (!rec || rec->empty()) {
                dropConn(connFd);
                return;
            }
            char tag = (*rec)[0];
            if (tag == MSG_QUERY_ACTIVE) {
                // A query connection never becomes a subscriber, so it takes no
                // refcount: answer and close.
                handleQueryActive(connFd);
                conn.done = true;
                return;
            }
            if (tag != MSG_START_OR_ATTACH) {
                dropConn(connFd);
                return;
            }
            try {
                handleStartOrAttach(connFd, *rec);
            } catch (std::exception & e) {
                printError("coordinator: bad START_OR_ATTACH: %s", e.what());
                dropConn(connFd);
            }
        } else {
            // Any readability after subscribing means the child closed = client
            // disconnect → refcounted unsubscribe.
            char b;
            ssize_t r = ::read(connFd, &b, 1);
            if (r <= 0)
                dropConn(connFd);
        }
    }

    void dropConn(int connFd)
    {
        auto it = conns.find(connFd);
        if (it == conns.end())
            return;
        if (it->second.started && !it->second.done)
            registry->unsubscribe(it->second.sub, DetachReason::Hup);
        conns.erase(it);
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
        int idleTicks = 0;
        while (true) {
            std::vector<pollfd> fds;
            fds.push_back({listenFd.get(), POLLIN, 0});
            for (auto & [fd, _] : conns)
                fds.push_back({fd, POLLIN, 0});
            for (auto & [fd, _] : running)
                fds.push_back({fd, POLLIN, 0});

            int n = ::poll(fds.data(), fds.size(), 10000);
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
                int c = ::accept(listenFd.get(), nullptr, nullptr);
                if (c >= 0) {
                    // Same-uid transport gate (defense in depth: the
                    // single-identity registry policy re-checks the identity).
                    if (auto uid = getPeerUid(c); uid && *uid == geteuid()) {
                        Conn conn;
                        conn.fd = AutoCloseFD{c};
                        conn.identity = std::to_string(*uid);
                        conns.emplace(c, std::move(conn));
                    } else
                        ::close(c);
                }
            }

            // Snapshot fds (handlers mutate the maps).
            std::vector<int> connReadable, buildReadable;
            for (size_t i = 1; i < fds.size(); ++i) {
                if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                int fd = fds[i].fd;
                if (conns.count(fd))
                    connReadable.push_back(fd);
                else if (running.count(fd))
                    buildReadable.push_back(fd);
            }
            for (int fd : buildReadable)
                if (running.count(fd))
                    onBuildPipeReadable(fd);
            for (int fd : connReadable)
                if (conns.count(fd))
                    onConnReadable(fd);

            // Close finished connections.
            for (auto it = conns.begin(); it != conns.end();) {
                if (it->second.done)
                    it = conns.erase(it);
                else
                    ++it;
            }
        }

        try {
            unlink(socketPath.c_str());
        } catch (...) {
        }
        _exit(0);
    }
};

/** Win the right to be *the* coordinator for this socket (the election):
 *  an exclusive lock on `${socketPath}.lock`. Returns a closed fd when
 *  another process holds the lock (it is — or is becoming — the
 *  coordinator); throws when the lock file cannot be used at all (an
 *  unusable socket location should fail loudly, not look like a lost
 *  election and be retried). */
AutoCloseFD electCoordinator(const std::string & socketPath)
{
    auto lockPath = socketPath + ".lock";
    AutoCloseFD lock{open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)};
    if (!lock)
        throw SysError("opening the coordinator election lock '%s'", lockPath);
    if (flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK)
            return {}; // someone else is (starting to be) the coordinator
        throw SysError("locking the coordinator election lock '%s'", lockPath);
    }
    return lock;
}

/** Spawn the coordinator process for an election already won: it inherits
 *  (and from then on owns) the held election lock and the already-listening
 *  control socket. The caller's copies of both fds close when the arguments
 *  go out of scope; the child's inherited descriptors keep the underlying
 *  open file descriptions — and thus the `flock` and the socket — alive. */
void spawnCoordinator(
    const std::string & socketPath, const std::string & storeUri, AutoCloseFD electionLock, AutoCloseFD listenFd)
{
    ProcessOptions opts;
    opts.dieWithParent = false; // outlives the spawning connection
    startProcess(
        [&]() {
            ::setsid();
            Coordinator coord{socketPath, storeUri, std::move(electionLock), std::move(listenFd)};
            coord.run(); // [[noreturn]]
        },
        opts);
}

/** One *non-blocking* attempt to reach the coordinator for `socketPath`,
 *  becoming its host if there is none: connect if one is up; otherwise run
 *  the election, and on a win bind + listen *here* — before spawning the
 *  coordinator — so the subsequent connect cannot race its startup (no
 *  sleep-and-retry handshake). Returns a closed fd only in the narrow
 *  lost-election race (another process is between taking the lock and
 *  binding); the caller decides how to wait before retrying. */
AutoCloseFD connectToCoordinator(const std::string & socketPath, const std::string & storeUri)
{
    // Fast path: a coordinator is already serving.
    try {
        return nix::connect(socketPath);
    } catch (SysError &) {
    }

    // Nothing serving: run the election ourselves.
    if (auto lock = electCoordinator(socketPath)) {
        // We won: any existing socket file is stale (a live coordinator
        // would hold the lock), so bind over it and hand both fds to the
        // spawned coordinator. Connections queue in the listen backlog
        // until it starts accepting.
        auto listenFd = createUnixDomainSocket(socketPath, 0600);
        spawnCoordinator(socketPath, storeUri, std::move(lock), std::move(listenFd));
        return nix::connect(socketPath);
    }

    // Lost the election. The winner binds before it spawns, so the window
    // in which the socket is not yet connectable is tiny: try once more,
    // and only genuinely mid-race failures bounce back to the caller.
    try {
        return nix::connect(socketPath);
    } catch (SysError &) {
        return {};
    }
}

} // namespace

struct CoordinatorRelayPump::Impl
{
    /* Emit `resBuildLogLine` results (not `log()`): the stderr tunnels forward
       results unconditionally, while `log()` is dropped at low verbosity —
       `nix-store --serve` pins `lvlError`, which would eat the whole log. */
    Activity act;

    /** Partially received line (frames may split lines, lines may span frames). */
    std::string pendingLine;

    /** Partially received length-prefixed record. */
    std::string pendingRecord;

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
        }
        return std::nullopt;
    }

    std::optional<BuildResult> feed(std::string_view data)
    {
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

std::optional<CoordinatorRelaySession> tryStartCoordinatorRelay(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted)
{
    auto fd = connectToCoordinator(coordinatorSocketPath(store), coordinatorStoreUri(store));
    if (!fd)
        return std::nullopt; // lost-election race; the caller schedules a retry

    // START_OR_ATTACH (keyed on the resolved drv — a BasicDerivation).
    {
        StringSink p;
        p << store.printStorePath(drvPath);
        writeDerivation(p, store, drv);
        p << (uint64_t) buildMode << (uint64_t) (trusted ? 1 : 0) << (uint64_t) 1 /*replayWanted*/;
        std::string body;
        body.push_back(MSG_START_OR_ATTACH);
        body += p.s;
        writeRecord(fd.get(), body);
    }

    return CoordinatorRelaySession{
        .socket = std::move(fd),
        .pump = CoordinatorRelayPump(logger, store.printStorePath(drvPath)),
    };
}

CoordinatorRelaySession startCoordinatorRelay(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        checkInterrupt();
        if (auto session = tryStartCoordinatorRelay(store, drvPath, drv, buildMode, logger, trusted))
            return std::move(*session);
        usleep(100000); // 100ms backoff for the lost-election race
    }
    throw Error("could not reach the build coordinator at '%s'", coordinatorSocketPath(store));
}

BuildResult relayBuildToCoordinator(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted)
{
    auto session = startCoordinatorRelay(store, drvPath, drv, buildMode, logger, trusted);

    // Relay frames to the ambient logger (→ the client) until the result.
    char buf[8192];
    while (true) {
        ssize_t r = ::read(session.socket.get(), buf, sizeof(buf));
        if (r == 0)
            throw Error("build coordinator closed the connection without a result");
        if (r < 0) {
            if (errno == EINTR) {
                checkInterrupt();
                continue;
            }
            throw SysError("reading from coordinator control socket");
        }
        if (auto res = session.pump.feed(std::string_view(buf, r)))
            return *res;
    }
}

BuildResult processCoordinatorRelayRecords(
    const std::function<std::optional<std::string>()> & readRecord, Logger & logger, const std::string & drvPathStr)
{
    CoordinatorRelayPump pump(logger, drvPathStr);
    while (auto rec = readRecord()) {
        // Re-frame the body with its length prefix, so this record-level
        // entry point exercises the same incremental decoder the event-loop
        // path uses.
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

    // Connect to an *existing* coordinator only — never spawn one for a query.
    // If nothing is listening (no coordinator up, or it idle-exited), there is
    // nothing in flight: report an empty list rather than an error.
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

void runBuildCoordinator(const std::string & socketPath, const std::string & storeUri)
{
    auto lock = electCoordinator(socketPath);
    if (!lock)
        _exit(0);
    auto listenFd = createUnixDomainSocket(socketPath, 0600);
    Coordinator coord{socketPath, storeUri, std::move(lock), std::move(listenFd)};
    coord.run();
}

} // namespace nix
