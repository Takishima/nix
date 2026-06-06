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

            int timeout = (conns.empty() && registry.empty()) ? IDLE_MS : -1;
            int n = ::poll(pfds.data(), pfds.size(), timeout);
            if (n < 0) { if (errno == EINTR) continue; throw std::runtime_error("poll"); }
            if (n == 0) { dbg("idle-exit"); break; }   // O3 idle exit

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

    // §3.7.2 authorization, re-derived by the coordinator (not delegated). For
    // the prototype: trusted callers may build anything; untrusted callers may
    // only build CA-style derivations (key prefix "ca:") they could build
    // themselves -- so an untrusted client cannot attach to an in-flight
    // input-addressed build it could not have requested (the A-trust property).
    static bool authorize(const SessionAuth & a, const std::string & drvForBuild)
    {
        if (a.trusted) return true;
        return drvForBuild.rfind("ca:", 0) == 0;
    }

    void onStartOrAttach(Conn & c, BufReader & r)
    {
        BuilderSpec spec;
        spec.buildKey    = r.str();
        spec.drvForBuild = r.str();
        SessionAuth auth{ r.u32(), r.u8() };
        bool replayWanted = r.u8() != 0;
        bool explicitRoot = r.u8() != 0;
        spec.counterFile = r.str();
        spec.nLines      = r.u32();
        spec.sleepMs     = r.u32();

        // authorize() BEFORE touching the registry (§3.3, §3.7). A denial is
        // byte-identical whether or not the build exists -> no existence oracle.
        if (!authorize(auth, spec.drvForBuild)) {
            dbg("DENIED start/attach uid " + std::to_string(auth.uid) + " key " + spec.buildKey);
            BufWriter w; w.u8(uint8_t(Msg::StartReply)); w.u8(uint8_t(Status::Denied)); w.u64(0); w.u8(0);
            enqueue(c, frame(w.buf));
            return;
        }

        c.auth = auth; c.replayWanted = replayWanted; c.buildKey = spec.buildKey;
        c.subId = nextSubId++;

        Build * b;
        auto it = registry.find(spec.buildKey);
        if (it == registry.end()) {                         // MISS -> start a build
            auto nb = std::make_unique<Build>();
            nb->key = spec.buildKey;
            nb->explicitRoot = explicitRoot;
            startBuilder(*nb, spec);
            c.deduplicated = false;
            b = nb.get();
            registry[spec.buildKey] = std::move(nb);
            dbg("MISS  key " + spec.buildKey + " -> started build pid " + std::to_string(b->builderPid));
        } else {                                            // HIT -> attach
            b = it->second.get();
            if (explicitRoot) b->explicitRoot = true;
            c.deduplicated = true;
            dbg("HIT   key " + spec.buildKey + " -> attaching (refcount was " + std::to_string(b->refcount) + ")");
        }
        b->refcount += 1;                                   // §3.3
        c.started = true;

        BufWriter w;
        w.u8(uint8_t(Msg::StartReply)); w.u8(uint8_t(Status::Ok)); w.u64(c.subId); w.u8(c.deduplicated ? 1 : 0);
        enqueue(c, frame(w.buf));
    }

    void startBuilder(Build & b, const BuilderSpec & spec)
    {
        b.logRef = stateDir + "/log/" + sanitize(b.key) + ".log";
        b.logFd = ::open(b.logRef.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0640);

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
            std::string self = selfDir();
            std::string script = self + "/slow-builder.sh";
            execlp("/bin/sh", "sh", script.c_str(),
                   spec.counterFile.c_str(),
                   spec.buildKey.c_str(),
                   std::to_string(spec.nLines).c_str(),
                   std::to_string(spec.sleepMs).c_str(),
                   (char *) nullptr);
            std::fprintf(stderr, "exec builder failed: %s\n", strerror(errno));
            _exit(127);
        }
        ::close(pipefd[1]);
        setNonBlocking(pipefd[0]);
        b.builderPid = pid;
        b.builderReadFd = pipefd[0];
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
        sendAttachState(c, AState::Building);
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

    void onQueryActive(Conn & c, BufReader & r)
    {
        SessionAuth auth{ r.u32(), r.u8() };
        // §3.7.3: filter to builds this caller is itself authorized to see.
        std::vector<Build *> visible;
        for (auto & [k, b] : registry)
            if (!b->reaped && authorize(auth, b->key)) visible.push_back(b.get());

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

    void maybeFinalize(Build & b)
    {
        if (!(b.reaped && b.builderEof)) return;            // wait for both
        bool ok = !b.cancelled && WIFEXITED(b.exitStatus) && WEXITSTATUS(b.exitStatus) == 0;
        int code = WIFEXITED(b.exitStatus) ? WEXITSTATUS(b.exitStatus) : -1;
        for (int fd : b.subscribers) {
            auto it = conns.find(fd);
            if (it == conns.end()) continue;
            Conn & c = *it->second;
            BufWriter w;
            w.u8(uint8_t(Msg::BuildResult));
            w.u8(ok ? 1 : 0); w.u32(uint32_t(code)); w.u8(c.deduplicated ? 1 : 0); w.str(b.logRef);
            enqueue(c, frame(w.buf));
            sendAttachState(c, AState::Finished);
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
