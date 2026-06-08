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

// child → coordinator
constexpr char MSG_START_OR_ATTACH = 'S';
// coordinator → child (or build-child → coordinator on the build pipe)
constexpr char MSG_FRAME = 'F';
constexpr char MSG_RESULT = 'R';

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
        // Build output lines arrive as resBuildLogLine (the same result the
        // daemon tunnels to the client); forward them as log frames.
        if (type == resBuildLogLine && !fields.empty() && fields[0].type == Logger::Field::tString)
            emit(fields[0].s);
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

/** Verify the connecting peer is the same uid as us — the pluggable-auth
 *  seam; peer-cred here, mTLS/identity in a network control plane. */
bool peerIsSameUid(int fd)
{
#ifdef SO_PEERCRED
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return false;
    return cred.uid == geteuid();
#else
    return true; // best-effort on non-Linux for this slice
#endif
}

struct Coordinator
{
    std::string socketPath;
    std::string storeUri;
    ref<Store> parseStore; // for parsing drvs/paths (parent never builds)
    AllowAllAuthPolicy policy;
    std::unique_ptr<BuildRegistry> registry = makeInMemoryBuildRegistry(policy);

    /** A connected daemon child = one subscription. */
    struct Conn
    {
        AutoCloseFD fd;
        bool started = false; // sent START_OR_ATTACH yet?
        bool done = false;    // result delivered; close after flush
        SubscriptionId sub;
        BuildRegistryKey key;
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

    Coordinator(std::string socketPath, std::string storeUri)
        : socketPath(std::move(socketPath))
        , storeUri(std::move(storeUri))
        , parseStore(openStore(this->storeUri))
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

        // The key is the resolved-drv path the child sent. For untrusted clients
        // the daemon already recomputed it from the drv (daemon.cc, the CA
        // `writeDerivation` path) before relaying, and the peer-cred check
        // limits connections to same-uid daemon children — so under the current
        // single-user experimental gate this is the daemon-validated key.
        // DEFERRED for the cross-user coordinator: the coordinator should
        // itself recompute the key from the received drv and
        // re-authorize every subscriber against the resolved key via a real
        // BuildAuthPolicy (replacing AllowAll), so a HIT cannot let one tenant
        // attach to another's build.
        BuildRegistryKey key{drvPathStr};
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

        BuildAuth auth{.identity = std::to_string(geteuid()), .trusted = trusted != 0};
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
            if (!rec || rec->empty() || (*rec)[0] != MSG_START_OR_ATTACH) {
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
        AutoCloseFD listenFd = createUnixDomainSocket(socketPath, 0600);

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
                    if (peerIsSameUid(c)) {
                        Conn conn;
                        conn.fd = AutoCloseFD{c};
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
 *  an exclusive lock on `${socketPath}.lock`. Losers exit; their relay connects
 *  to the winner. */
AutoCloseFD electCoordinator(const std::string & socketPath)
{
    auto lockPath = socketPath + ".lock";
    AutoCloseFD lock{open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)};
    if (!lock)
        return {};
    if (flock(lock.get(), LOCK_EX | LOCK_NB) != 0)
        return {}; // someone else is (starting to be) the coordinator
    return lock;
}

void spawnCoordinator(const std::string & socketPath, const std::string & storeUri)
{
    ProcessOptions opts;
    opts.dieWithParent = false; // outlives the spawning connection
    startProcess(
        [&]() {
            ::setsid();
            auto lock = electCoordinator(socketPath);
            if (!lock)
                return; // lost the election; the winner serves
            Coordinator coord{socketPath, storeUri};
            coord.run(); // [[noreturn]]
        },
        opts);
}

} // namespace

BuildResult relayBuildToCoordinator(
    Store & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    BuildMode buildMode,
    Logger & logger,
    bool trusted)
{
    auto storeUri = coordinatorStoreUri(store);
    auto socketPath = coordinatorSocketPath(store);
    // Connect, lazily spawning the coordinator if absent (decline-and-respawn).
    AutoCloseFD fd;
    for (int attempt = 0; attempt < 50; ++attempt) {
        checkInterrupt();
        try {
            fd = nix::connect(socketPath);
            break;
        } catch (SysError &) {
            if (attempt == 0)
                spawnCoordinator(socketPath, storeUri);
            usleep(100000); // 100ms backoff while it comes up
        }
    }
    if (!fd)
        throw Error("could not reach the build coordinator at '%s'", socketPath);

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

    // Relay frames to the ambient logger (→ the client) until the result.
    while (true) {
        auto rec = readRecord(fd.get());
        if (!rec)
            throw Error("build coordinator closed the connection without a result");
        if (rec->empty())
            continue;
        char tag = (*rec)[0];
        if (tag == MSG_FRAME) {
            logger.log(lvlInfo, std::string_view(*rec).substr(2));
        } else if (tag == MSG_RESULT) {
            return nlohmann::json::parse(rec->substr(1)).get<BuildResult>();
        }
    }
}

void runBuildCoordinator(const std::string & socketPath, const std::string & storeUri)
{
    auto lock = electCoordinator(socketPath);
    if (!lock)
        _exit(0);
    Coordinator coord{socketPath, storeUri};
    coord.run();
}

} // namespace nix
