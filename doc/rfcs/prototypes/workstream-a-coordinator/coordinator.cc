// Workstream A prototype — the coordinator (deliverable A1).
//
// THROW-AWAY CODE (see proto.hh header). This models spike §3: a long-lived,
// per-store coordinator that owns the Build Registry, the per-build in-memory
// replay buffer (§3.5, O5 head+tail cap), the subscriber refcounts (§3.4), and
// runs the actual build by fork()ing a builder subprocess. Connection children
// (reldaemon.cc) are thin relays.
//
// What this binary demonstrates, mapped to the validation plan's acceptance
// criteria (validation.md, Workstream A):
//   A-dedup        one Build per key; second START_OR_ATTACH is a HIT      (§3.3)
//   A-replay       late joiner gets replayed=true prefix then the live tail (§3.5)
//   A-backpressure a stalled subscriber is demoted, the build is not        (§3.6)
//   A-sockauth     SO_PEERCRED uid check before any sessionAuth is read     (§3.7.1)
//   A-crash        builders die with the coordinator; relays fall back      (O2)
//   A-spawn        single-writer election lives in reldaemon.cc             (O3)
//
// Design choices it bakes in: single-threaded event loop (O4), in-coordinator
// replay buffer (RFC Q1), authorize-before-subscribe with re-derived
// authorization (§3.7.2), refcounted cancel with an explicit-root escape
// (§5.2 / C-c).

#include "proto.hh"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <time.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>

using namespace wsa;

namespace {

// ---- tunables (overridable from the environment for the tests) -------------

size_t envSize(const char * name, size_t dflt)
{
    if (const char * v = ::getenv(name)) { try { return std::stoul(v); } catch (...) {} }
    return dflt;
}

bool gDebug = ::getenv("WSA_DEBUG") != nullptr;

void dbg(const std::string & s)
{
    if (gDebug) std::fprintf(stderr, "[coordinator %d] %s\n", ::getpid(), s.c_str());
}

const size_t HEAD_CAP = envSize("WSA_HEAD_CAP", 1u << 20);   // O5: ~1 MiB head
const size_t TAIL_CAP = envSize("WSA_TAIL_CAP", 3u << 20);   // O5: ~3 MiB tail
const size_t OUT_CAP  = envSize("WSA_OUT_CAP", 256u << 10);  // §3.6 per-subscriber cap
const int    IDLE_MS  = int(envSize("WSA_IDLE_MS", 600000)); // O3: idle-exit grace (10 min)

long nowMs()
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return long(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string sanitize(const std::string & key)
{
    std::string out;
    for (char c : key) out.push_back((std::isalnum((unsigned char) c) || c == '-' || c == '_') ? c : '_');
    return out;
}

// ---- a single shared build (spike §3.1 "Build") ----------------------------

struct Build
{
    std::string key;
    pid_t builderPid = -1;
    int builderReadFd = -1;     // pipe from the forked builder's stdout
    bool builderEof = false;
    bool reaped = false;
    int exitStatus = 0;
    bool cancelled = false;
    bool explicitRoot = false;  // §5.2: keeps the build alive at refcount 0 (C-c)

    // Workstream B / CA resolve+promote (spike §3.8).
    AState state = AState::Building;  // IA builds start Building; CA start Resolving
    std::string resolvedDrv;    // canonical resolved-key material (re-auth target)
    std::string authMaterial;   // drv material QUERY_ACTIVE filters against
    long resolveDeadlineMs = 0; // when the resolve phase promotes (steady ms)
    // builder params, captured so a CA build can start at *promotion*, not attach:
    std::string counterFile; uint32_t nLines = 0; uint32_t sleepMs = 0;
    // Workstream C / Blocker 2:
    uint32_t failAt = 0;        // builder fails at this line (C-e)
    bool keepFailedOR = false;  // --keep-failed is a logical OR across subscribers
    bool timedOutCancel = false;// cancelled because every subscriber's deadline elapsed
    std::string workdir;        // stand-in for the failed build dir (kept iff keepFailedOR)

    int refcount = 0;
    std::vector<int> subscribers;     // live subscriber connection fds

    // Replay buffer (§3.5, O5): full log up to a head+tail byte cap.
    std::vector<std::string> head; size_t headBytes = 0;
    std::deque<std::string>  tail; size_t tailBytes = 0;
    size_t truncated = 0;

    int logFd = -1;             // persisted log writer (§4.4 hand-off boundary)
    std::string logRef;

    void appendReplay(const std::string & chunk)
    {
        if (headBytes < HEAD_CAP) { headBytes += chunk.size(); head.push_back(chunk); }
        else {
            tailBytes += chunk.size(); tail.push_back(chunk);
            while (tailBytes > TAIL_CAP && !tail.empty()) {
                tailBytes -= tail.front().size(); tail.pop_front(); ++truncated;
            }
        }
    }
};

// ---- a connection from a relay child (spike §3.2) --------------------------

struct Conn
{
    int fd = -1;
    PeerCred cred{};
    std::string inbuf;          // accumulating framed input
    std::string outbuf;         // pending output (backpressure lives here)

    bool subscribed = false;    // live-subscribed to a build?
    bool demoted = false;       // §3.6: dropped to persisted-log fallback
    uint64_t subId = 0;
    std::string buildKey;
    SessionAuth auth{};
    bool deduplicated = false;
    bool replayWanted = false;
    bool started = false;       // START_OR_ATTACH succeeded (refcount held)
    long deadlineMs = 0;        // Workstream C: per-subscriber timeout (0 = none)
};

// ---- the coordinator -------------------------------------------------------

class Coordinator
{
    std::string stateDir;
    std::string sockPath;
    uid_t daemonUid;            // the only uid allowed on the control socket
    int listenFd = -1;
    int sigFd = -1;
    uint64_t nextSubId = 1;

    std::unordered_map<int, std::unique_ptr<Conn>> conns;
    std::unordered_map<std::string, std::unique_ptr<Build>> registry;

public:
    Coordinator(std::string sd) : stateDir(std::move(sd))
    {
        sockPath = stateDir + "/coordinator.socket";
        // §3.7.1: only the daemon's own uid may speak the control protocol.
        // WSA_FAKE_DAEMON_UID lets the A-sockauth test force a uid mismatch
        // without needing a second real user account.
        daemonUid = ::getenv("WSA_FAKE_DAEMON_UID")
            ? uid_t(std::stoul(::getenv("WSA_FAKE_DAEMON_UID"))) : ::getuid();
        ::mkdir((stateDir + "/log").c_str(), 0750);
    }

    void run()
    {
        listenFd = makeListenSocket(sockPath);
        setNonBlocking(listenFd);

        sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGCHLD);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        sigFd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (sigFd < 0) throw std::runtime_error("signalfd");
        signal(SIGPIPE, SIG_IGN);

        dbg("listening on " + sockPath + " (uid " + std::to_string(daemonUid) + ")");

        for (;;) {
            std::vector<pollfd> pfds;
            pfds.push_back({listenFd, POLLIN, 0});
            pfds.push_back({sigFd, POLLIN, 0});
            for (auto & [fd, c] : conns) {
                short ev = POLLIN;
                if (!c->outbuf.empty()) ev |= POLLOUT;
                pfds.push_back({fd, ev, 0});
            }
            for (auto & [k, b] : registry)
                if (b->builderReadFd >= 0) pfds.push_back({b->builderReadFd, POLLIN, 0});

            // Wake in time for the soonest timed event: a CA resolve→promote
            // deadline (§3.8) or a subscriber timeout (Blocker 2).
            long soonest = -1;
            auto bump = [&](long d) { if (d) soonest = (soonest < 0) ? d : std::min(soonest, d); };
            for (auto & [k, b] : registry)
                if (b->state == AState::Resolving) bump(b->resolveDeadlineMs);
            for (auto & [fd, c] : conns)
                if (c->subscribed && c->deadlineMs) bump(c->deadlineMs);
            int timeout;
            if (soonest >= 0) timeout = std::max(0, int(soonest - nowMs()));
            else timeout = (conns.empty() && registry.empty()) ? IDLE_MS : -1;

            int n = ::poll(pfds.data(), pfds.size(), timeout);
            if (n < 0) { if (errno == EINTR) continue; throw std::runtime_error("poll"); }
            checkPromotions();
            checkDeadlines();
            if (n == 0 && soonest < 0) { dbg("idle-exit"); break; }   // O3 idle exit
            if (n == 0) continue;

            for (auto & p : pfds) {
                if (!p.revents) continue;
                if (p.fd == listenFd) onAccept();
                else if (p.fd == sigFd) onSigchld();
                else if (auto it = conns.find(p.fd); it != conns.end()) {
                    if (p.revents & POLLOUT) flush(*it->second);
                    if (conns.count(p.fd) && (p.revents & (POLLIN | POLLHUP)))
                        onConnReadable(p.fd);
                } else if (p.revents & (POLLIN | POLLHUP))
                    onBuilderReadable(p.fd);
            }
        }
        ::unlink(sockPath.c_str());
    }

private:
    // §3.7.1: peer-credential check BEFORE a byte of sessionAuth is read.
    void onAccept()
    {
        for (;;) {
            int fd = ::accept(listenFd, nullptr, nullptr);
            if (fd < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; if (errno == EINTR) continue; break; }
            PeerCred cred = getPeerCred(fd);
            if (cred.uid != daemonUid) {
                dbg("REFUSED control connection from uid " + std::to_string(cred.uid)
                    + " (expected " + std::to_string(daemonUid) + ")");   // A-sockauth
                ::close(fd);
                continue;
            }
            setSockBuf(fd);
            setNonBlocking(fd);
            auto c = std::make_unique<Conn>();
            c->fd = fd; c->cred = cred;
            conns[fd] = std::move(c);
            dbg("accepted control connection fd " + std::to_string(fd) + " uid " + std::to_string(cred.uid));
        }
    }

    void onSigchld()
    {
        signalfd_siginfo si;
        while (::read(sigFd, &si, sizeof(si)) == sizeof(si)) {}
        int status; pid_t pid;
        while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
            for (auto & [k, b] : registry) {
                if (b->builderPid == pid) {
                    b->reaped = true; b->exitStatus = status;
                    dbg("builder for " + k + " exited status " + std::to_string(status));
                    maybeFinalize(*b);
                    break;
                }
            }
        }
    }

    void onConnReadable(int fd)
    {
        auto it = conns.find(fd);
        if (it == conns.end()) return;
        Conn & c = *it->second;
        char tmp[8192];
        for (;;) {
            ssize_t n = ::read(fd, tmp, sizeof(tmp));
            if (n > 0) { c.inbuf.append(tmp, size_t(n)); continue; }
            if (n == 0) { dropConn(fd); return; }                     // client/relay gone
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            dropConn(fd); return;
        }
        while (auto body = takeFrame(c.inbuf)) dispatch(c, *body);
    }

    void dispatch(Conn & c, const std::string & body)
    {
        BufReader r{body};
        Op op = Op(r.u8());
        switch (op) {
        case Op::StartOrAttach: onStartOrAttach(c, r); break;
        case Op::Subscribe:     onSubscribe(c, r.u64()); break;
        case Op::Unsubscribe:   onUnsubscribe(c); break;
        case Op::CancelHint:    onUnsubscribe(c); break;   // §3.4: hint == detach in the prototype
        case Op::QueryActive:   onQueryActive(c, r); break;
        default: dbg("unknown op"); break;
        }
    }

    void denyStart(Conn & c)
    {
        // A denial is byte-identical whether or not the build exists, and is
        // produced before any registry lookup -> no existence/timing oracle (T1).
        BufWriter w; w.u8(uint8_t(Msg::StartReply)); w.u8(uint8_t(Status::Denied)); w.u64(0); w.u8(0);
        enqueue(c, frame(w.buf));
    }

    void onStartOrAttach(Conn & c, BufReader & r)
    {
        std::string assertedKey = r.str();
        std::string drvForBuild = r.str();
        SessionAuth auth{ r.u32(), r.u8() };
        bool replayWanted = r.u8() != 0;
        bool explicitRoot = r.u8() != 0;
        std::string counterFile = r.str();
        uint32_t nLines = r.u32();
        uint32_t sleepMs = r.u32();
        bool ca = r.u8() != 0;
        std::string unresolvedDrv = r.str();
        std::string resolvedDrv = r.str();
        uint32_t resolveMs = r.u32();
        uint32_t timeoutMs = r.u32();
        bool keepFailed = r.u8() != 0;
        uint32_t failAt = r.u32();

        // The drv material the caller is authorized against *at attach time*:
        // the unresolved drv for CA (the resolved key is not known yet), the drv
        // itself for IA.
        std::string attachMaterial = ca ? unresolvedDrv : drvForBuild;

        // authorize BEFORE touching the registry (§3.3, §3.7 / T1).
        if (!authorizeFor(auth, attachMaterial)) {
            dbg("DENIED start/attach uid " + std::to_string(auth.uid) + " (no auth for " + attachMaterial + ")");
            denyStart(c);
            return;
        }

        // T3: the coordinator recomputes the canonical key from the drv material
        // it received and rejects a child that asserts a different key. (For IA
        // the canonical key is the drv; for CA it is the resolved drv.)
        std::string canonicalKey = ca ? resolvedDrv : drvForBuild;
        if (!assertedKey.empty() && assertedKey != canonicalKey) {
            dbg("REJECT asserted key mismatch: child said '" + assertedKey + "' but drv resolves to '" + canonicalKey + "'");
            denyStart(c);
            return;
        }

        c.auth = auth; c.replayWanted = replayWanted; c.subId = nextSubId++;
        c.deadlineMs = timeoutMs ? (nowMs() + long(timeoutMs)) : 0;   // per-subscriber deadline

        // Registry key: a CA build lives under a provisional "unresolved:" key
        // during the resolve phase (spike §3.8); IA goes straight to its key.
        std::string regKey = ca ? ("unresolved:" + unresolvedDrv) : drvForBuild;
        c.buildKey = regKey;

        Build * b;
        auto it = registry.find(regKey);
        if (it == registry.end()) {                         // MISS
            auto nb = std::make_unique<Build>();
            nb->key = regKey;
            nb->explicitRoot = explicitRoot;
            nb->counterFile = counterFile; nb->nLines = nLines; nb->sleepMs = sleepMs;
            nb->failAt = failAt; nb->keepFailedOR = keepFailed;
            nb->resolvedDrv = resolvedDrv;
            nb->authMaterial = ca ? resolvedDrv : drvForBuild;
            b = nb.get();
            if (ca) {                                       // start a `resolving` pre-state
                b->state = AState::Resolving;
                b->resolveDeadlineMs = nowMs() + long(resolveMs);
                registry[regKey] = std::move(nb);
                dbg("MISS  CA " + unresolvedDrv + " -> resolving (promote in " + std::to_string(resolveMs) + "ms)");
                onBuildOutput(*b, "[resolving CA derivation " + unresolvedDrv + "]\n");
            } else {                                        // IA: build immediately
                startBuilder(*nb);
                registry[regKey] = std::move(nb);
                dbg("MISS  IA " + regKey + " -> started build pid " + std::to_string(b->builderPid));
            }
            c.deduplicated = false;
        } else {                                            // HIT -> attach
            b = it->second.get();
            if (explicitRoot) b->explicitRoot = true;
            if (keepFailed) b->keepFailedOR = true;         // §3: keep-failed is an OR
            c.deduplicated = true;
            dbg("HIT   " + regKey + " -> attaching (refcount was " + std::to_string(b->refcount) + ")");
        }
        b->refcount += 1;                                   // §3.3
        c.started = true;

        BufWriter w;
        w.u8(uint8_t(Msg::StartReply)); w.u8(uint8_t(Status::Ok)); w.u64(c.subId); w.u8(c.deduplicated ? 1 : 0);
        enqueue(c, frame(w.buf));
    }

    // Fork the builder for an already-keyed Build (IA: at miss; CA: at promotion).
    void startBuilder(Build & b)
    {
        b.logRef = stateDir + "/log/" + sanitize(b.key) + ".log";
        b.logFd = ::open(b.logRef.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);
        // The build's working directory; on failure it is kept iff --keep-failed
        // was requested by ≥1 attached subscriber (Blocker 2 §3, the OR rule).
        b.workdir = b.logRef + ".workdir";
        ::mkdir(b.workdir.c_str(), 0750);

        int pipefd[2];
        if (::pipe(pipefd) < 0) throw std::runtime_error("pipe");

        pid_t pid = ::fork();
        if (pid < 0) throw std::runtime_error("fork");
        if (pid == 0) {                                     // builder child
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::dup2(pipefd[1], STDERR_FILENO);
            ::close(pipefd[0]); ::close(pipefd[1]);
            // O2 / A-crash: die with the coordinator so a coordinator crash
            // never leaves an orphan builder holding output locks.
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            std::string script = selfDir() + "/slow-builder.sh";
            execlp("/bin/sh", "sh", script.c_str(),
                   b.counterFile.c_str(),
                   b.key.c_str(),
                   std::to_string(b.nLines).c_str(),
                   std::to_string(b.sleepMs).c_str(),
                   std::to_string(b.failAt).c_str(),
                   (char *) nullptr);
            std::fprintf(stderr, "exec builder failed: %s\n", strerror(errno));
            _exit(127);
        }
        ::close(pipefd[1]);
        setNonBlocking(pipefd[0]);
        b.builderPid = pid;
        b.builderReadFd = pipefd[0];
        b.state = AState::Building;
    }

    void onSubscribe(Conn & c, uint64_t subId)
    {
        auto it = registry.find(c.buildKey);
        if (it == registry.end() || !c.started || subId != c.subId) return;
        Build & b = *it->second;

        // §3.5 atomic seam: snapshot the replay buffer into this subscriber's
        // queue (tagged replayed=true), then register as live. Single-threaded,
        // so nothing slips in between -> no gap or dup at the handoff.
        if (c.replayWanted) {
            for (auto & chunk : b.head) sendFrame(c, chunk, /*replayed=*/true);
            if (b.truncated)
                sendFrame(c, "\n...[" + std::to_string(b.truncated) + " frames truncated]...\n", true);
            for (auto & chunk : b.tail) sendFrame(c, chunk, true);
        }
        sendAttachState(c, b.state);
        b.subscribers.push_back(c.fd);
        c.subscribed = true;
        dbg("subscribed fd " + std::to_string(c.fd) + " to " + c.buildKey
            + (c.replayWanted ? " (with replay)" : ""));
    }

    void onUnsubscribe(Conn & c)
    {
        if (!c.started) return;
        auto it = registry.find(c.buildKey);
        if (it == registry.end()) { c.started = false; return; }
        Build & b = *it->second;

        auto & subs = b.subscribers;
        subs.erase(std::remove(subs.begin(), subs.end(), c.fd), subs.end());
        b.refcount -= 1;
        c.subscribed = false; c.started = false;
        dbg("unsubscribe fd " + std::to_string(c.fd) + " key " + c.buildKey
            + " refcount -> " + std::to_string(b.refcount));

        // §3.4 refcounted cancellation (the inverted MonitorFdHup semantics).
        if (b.refcount <= 0 && !hasRootReasonToContinue(b)) {
            dbg("refcount 0, no root reason -> cancelling " + b.key);
            cancelBuild(b);
        }
    }

    // §5.2 stub: only an explicit build root keeps a refcount-0 build alive.
    // (--keep-going / timeouts are the deferred Q2 matrix.)
    static bool hasRootReasonToContinue(const Build & b) { return b.explicitRoot; }

    // ---- Workstream B: CA resolve -> promote/merge + re-auth (spike §3.8) ---

    void checkPromotions()
    {
        long t = nowMs();
        std::vector<std::string> due;
        for (auto & [k, b] : registry)
            if (b->state == AState::Resolving && t >= b->resolveDeadlineMs) due.push_back(k);
        for (auto & k : due) promote(k);
    }

    // Workstream C / Blocker 2: per-subscriber deadlines under a max envelope.
    // A subscriber whose own deadline elapses gets a TimedOut BuildResult and
    // detaches (refcount--) WITHOUT cancelling the build for others; the build
    // thus runs under the max of the remaining deadlines. When the *last*
    // subscriber times out, refcount hits 0 and the normal no-root cancel fires
    // -- i.e. "all deadlines elapsed → cancel (timeout)".
    void checkDeadlines()
    {
        long t = nowMs();
        std::vector<int> expired;
        for (auto & [fd, c] : conns)
            if (c->subscribed && c->deadlineMs && t >= c->deadlineMs) expired.push_back(fd);
        for (int fd : expired) {
            auto it = conns.find(fd);
            if (it == conns.end()) continue;
            Conn & c = *it->second;
            dbg("TIMEOUT subscriber fd " + std::to_string(fd) + " key " + c.buildKey);
            // Mark a timeout cancel iff this is the last subscriber (so the
            // resulting cancel is attributable to timeout, not plain detach).
            auto bi = registry.find(c.buildKey);
            if (bi != registry.end() && bi->second->refcount <= 1) bi->second->timedOutCancel = true;
            sendBuildResult(c, ResultStatus::TimedOut, 0, "");
            onUnsubscribe(c);   // detach, refcount--, cancel iff 0 and no root
        }
    }

    // The resolve phase completed: re-authorize every subscriber against the
    // *resolved* key (decisions B1 §5 / T2), detaching failures with an error,
    // then promote — merging onto an existing real Build under the same resolved
    // key if one exists (CA-coalescing), else re-keying this entry and starting
    // the builder. Detached subscribers never see the build's log (T2).
    void promote(const std::string & provKey)
    {
        auto it = registry.find(provKey);
        if (it == registry.end()) return;
        Build & prov = *it->second;
        std::string resolvedKey = prov.resolvedDrv;
        dbg("PROMOTE " + provKey + " -> resolved key " + resolvedKey);

        // Re-authorize each subscriber against the resolved drv (§3.8 / T2).
        std::vector<int> survivors;
        for (int fd : prov.subscribers) {
            auto ci = conns.find(fd);
            if (ci == conns.end()) continue;
            Conn & sc = *ci->second;
            if (authorizeFor(sc.auth, prov.resolvedDrv)) {
                survivors.push_back(fd);
            } else {
                dbg("RE-AUTH FAILED at promotion for fd " + std::to_string(fd)
                    + " uid " + std::to_string(sc.auth.uid) + " -> detaching with error");
                // The subscriber observes only an error -- never A's build log.
                onBuildOutputTo(sc, "[re-authorization failed at CA promotion: not authorized for the resolved key]\n");
                sendBuildResult(sc, ResultStatus::Failure, 13 /*EACCES-ish*/, "");
                sc.subscribed = false; sc.started = false;
            }
        }

        auto existing = registry.find(resolvedKey);
        if (existing != registry.end() && existing->second->state == AState::Building) {
            // MERGE onto the already-running resolved build (CA-coalescing, §3.8).
            Build & real = *existing->second;
            for (int fd : survivors) {
                auto ci = conns.find(fd);
                if (ci == conns.end()) continue;
                Conn & sc = *ci->second;
                sc.buildKey = resolvedKey; sc.deduplicated = true;
                if (sc.replayWanted) {                      // catch the joiner up
                    for (auto & ch : real.head) sendFrame(sc, ch, true);
                    for (auto & ch : real.tail) sendFrame(sc, ch, true);
                }
                sendAttachState(sc, AState::Building);
                real.subscribers.push_back(fd);
                real.refcount += 1;
            }
            registry.erase(provKey);                        // drop the provisional entry
            dbg("merged " + std::to_string(survivors.size()) + " subscriber(s) into running " + resolvedKey);
        } else {
            // Re-key the provisional entry to the resolved key and start the build.
            auto node = std::move(it->second);
            registry.erase(provKey);
            node->key = resolvedKey;
            node->subscribers = survivors;
            node->refcount = int(survivors.size());
            for (int fd : survivors) {
                auto ci = conns.find(fd);
                if (ci != conns.end()) { ci->second->buildKey = resolvedKey; sendAttachState(*ci->second, AState::Building); }
            }
            Build * b = node.get();
            registry[resolvedKey] = std::move(node);
            if (b->refcount > 0) {
                startBuilder(*b);
                dbg("promoted to build " + resolvedKey + " pid " + std::to_string(b->builderPid));
            } else {
                // everyone was de-authorized; nothing to build
                registry.erase(resolvedKey);
                dbg("promotion left no authorized subscribers; dropped " + resolvedKey);
            }
        }
    }

    // Send a single synthetic log line to exactly one subscriber (used to deliver
    // the re-auth error without touching the shared build's fan-out).
    void onBuildOutputTo(Conn & c, const std::string & text) { sendFrame(c, text, false); }

    void onQueryActive(Conn & c, BufReader & r)
    {
        SessionAuth auth{ r.u32(), r.u8() };
        // §3.7.3: filter to builds this caller is itself authorized to see;
        // provisional `resolving` entries are not advertised (no unresolved-drv leak).
        std::vector<Build *> visible;
        for (auto & [k, b] : registry)
            if (!b->reaped && b->state == AState::Building && authorizeFor(auth, b->authMaterial))
                visible.push_back(b.get());

        BufWriter w; w.u8(uint8_t(Msg::ActiveList)); w.u32(uint32_t(visible.size()));
        for (Build * b : visible) {
            w.str(b->key);
            w.u32(uint32_t(b->subscribers.size()));
            w.u32(uint32_t(b->headBytes + b->tailBytes));
        }
        enqueue(c, frame(w.buf));
    }

    void onBuilderReadable(int fd)
    {
        Build * b = nullptr;
        for (auto & [k, bb] : registry) if (bb->builderReadFd == fd) { b = bb.get(); break; }
        if (!b) return;
        char tmp[8192];
        for (;;) {
            ssize_t n = ::read(fd, tmp, sizeof(tmp));
            if (n > 0) { onBuildOutput(*b, std::string(tmp, size_t(n))); continue; }
            if (n == 0) { b->builderEof = true; ::close(fd); b->builderReadFd = -1; maybeFinalize(*b); return; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            b->builderEof = true; ::close(fd); b->builderReadFd = -1; maybeFinalize(*b); return;
        }
    }

    void onBuildOutput(Build & b, const std::string & chunk)
    {
        b.appendReplay(chunk);                              // replay buffer
        if (b.logFd >= 0) { auto _ = ::write(b.logFd, chunk.data(), chunk.size()); (void) _; }  // persisted log
        for (int fd : b.subscribers) {                      // live fan-out
            auto it = conns.find(fd);
            if (it != conns.end()) sendFrame(*it->second, chunk, /*replayed=*/false);
        }
    }

    void sendBuildResult(Conn & c, ResultStatus st, int code, const std::string & logRef)
    {
        BufWriter w; w.u8(uint8_t(Msg::BuildResult));
        w.u8(uint8_t(st)); w.u32(uint32_t(code)); w.u8(c.deduplicated ? 1 : 0); w.str(logRef);
        enqueue(c, frame(w.buf));
    }

    void maybeFinalize(Build & b)
    {
        if (!(b.reaped && b.builderEof)) return;            // wait for both
        bool ok = !b.cancelled && WIFEXITED(b.exitStatus) && WEXITSTATUS(b.exitStatus) == 0;
        int code = WIFEXITED(b.exitStatus) ? WEXITSTATUS(b.exitStatus) : -1;
        ResultStatus st = ok ? ResultStatus::Success : ResultStatus::Failure;

        // Blocker 2 §3: keep the failed build dir iff it was a genuine build
        // failure (not a cancel) AND ≥1 attached subscriber asked (--keep-failed
        // OR). Success always cleans up.
        if (!b.workdir.empty()) {
            bool keep = (!ok && !b.cancelled && b.keepFailedOR);
            if (!keep) ::rmdir(b.workdir.c_str());
            dbg("workdir " + b.workdir + (keep ? " PRESERVED (keep-failed OR)" : " cleaned"));
        }

        for (int fd : b.subscribers) {
            auto it = conns.find(fd);
            if (it == conns.end()) continue;
            sendBuildResult(*it->second, st, code, b.logRef);
            sendAttachState(*it->second, AState::Finished);
            it->second->subscribed = false; it->second->started = false;  // no post-finalize timeout
        }
        if (b.logFd >= 0) { ::close(b.logFd); b.logFd = -1; }
        std::string key = b.key;
        dbg("finalized " + key + (ok ? " OK" : " FAIL") + " -> dropping registry entry");
        registry.erase(key);                                // drop entry (§3.3)
    }

    void cancelBuild(Build & b)
    {
        b.cancelled = true;
        if (b.builderPid > 0) ::kill(b.builderPid, SIGKILL);
        // The SIGCHLD reap + builder EOF will run maybeFinalize and drop the entry.
    }

    // ---- per-subscriber output with backpressure (§3.6) --------------------

    void sendFrame(Conn & c, const std::string & chunk, bool replayed)
    {
        if (c.demoted) return;
        BufWriter w; w.u8(uint8_t(Msg::Frame)); w.u8(replayed ? 1 : 0); w.str(chunk);
        enqueue(c, frame(w.buf));
    }

    void sendAttachState(Conn & c, AState st)
    {
        if (c.demoted) return;
        BufWriter w; w.u8(uint8_t(Msg::AttachState)); w.u8(uint8_t(st));
        enqueue(c, frame(w.buf));
    }

    // Queue bytes for a connection; flush what we can immediately. If a live
    // subscriber's queue blows past OUT_CAP it is a slow client: demote it
    // (§3.6) -- stop feeding the live tail, but leave the build and every other
    // subscriber untouched. The build is NEVER throttled by a slow client.
    void enqueue(Conn & c, const std::string & bytes)
    {
        c.outbuf.append(bytes);
        flush(c);
        if (c.subscribed && !c.demoted && c.outbuf.size() > OUT_CAP) {
            dbg("DEMOTING slow subscriber fd " + std::to_string(c.fd)
                + " (queue " + std::to_string(c.outbuf.size()) + " > cap " + std::to_string(OUT_CAP) + ")");
            // Remove from live fan-out; keep the connection + refcount so the
            // build keeps running and the client can fall back to the log.
            auto it = registry.find(c.buildKey);
            if (it != registry.end()) {
                auto & subs = it->second->subscribers;
                subs.erase(std::remove(subs.begin(), subs.end(), c.fd), subs.end());
            }
            c.demoted = true;
            BufWriter w; w.u8(uint8_t(Msg::Frame)); w.u8(1);
            w.str("\n...[demoted: too slow; fall back to persisted log " + c.buildKey + "]...\n");
            c.outbuf.append(frame(w.buf));
            flush(c);
        }
    }

    void flush(Conn & c)
    {
        while (!c.outbuf.empty()) {
            ssize_t n = ::write(c.fd, c.outbuf.data(), c.outbuf.size());
            if (n > 0) { c.outbuf.erase(0, size_t(n)); continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // socket full -> backpressure
            if (n < 0 && errno == EINTR) continue;
            break;  // EPIPE etc.; the POLLHUP path will drop the conn
        }
    }

    void dropConn(int fd)
    {
        auto it = conns.find(fd);
        if (it == conns.end()) return;
        Conn & c = *it->second;
        dbg("dropping conn fd " + std::to_string(fd));
        if (c.started) onUnsubscribe(c);   // disconnect == unsubscribe (§3.4)
        ::close(fd);
        conns.erase(fd);
    }

    static std::string selfDir()
    {
        char buf[4096];
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return ".";
        buf[n] = 0;
        std::string p(buf);
        auto slash = p.find_last_of('/');
        return slash == std::string::npos ? "." : p.substr(0, slash);
    }
};

} // namespace

int main(int argc, char ** argv)
{
    std::string stateDir = (argc > 1) ? argv[1] : (::getenv("WSA_STATE_DIR") ?: "/tmp/wsa");
    try {
        Coordinator(stateDir).run();
    } catch (const std::exception & e) {
        std::fprintf(stderr, "[coordinator] fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
