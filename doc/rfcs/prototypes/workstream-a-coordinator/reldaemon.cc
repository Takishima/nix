// Workstream A prototype — the relay daemon (deliverable A2).
//
// THROW-AWAY CODE (see proto.hh). Models spike §1.1 + §3.4: the stock
// fork-per-connection daemon, with the one new behaviour the spike adds — for a
// build request, instead of building in-process the forked child opens a control
// connection to the coordinator (START_OR_ATTACH + SUBSCRIBE) and relays the
// coordinator's FRAMEs down its own client socket "verbatim" (§3.4), so the
// public wire is unchanged (§5.1).
//
// It also carries the two pieces that live on the child side, not the
// coordinator:
//   * lazy-spawn / single-writer election (O3 / spike §3.1)  -> ensureCoordinator()
//   * coordinator-crash fallback to a local, PathLock-coalesced build (O2 / §5.1)
//     -> fallbackLocalBuild(); proves A-crash's "exactly one rebuild".
//
// The inverted MonitorFdHup (§1.5 -> §3.4): a client HUP makes the child send
// UNSUBSCRIBE/CANCEL_HINT, NOT triggerInterrupt -- that is what lets the
// coordinator keep the build alive for other subscribers (refcounted cancel).

#include "proto.hh"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>

#include <poll.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>

using namespace wsa;

namespace {

bool gDebug = ::getenv("WSA_DEBUG") != nullptr;
void dbg(const std::string & s) { if (gDebug) std::fprintf(stderr, "[reldaemon %d] %s\n", ::getpid(), s.c_str()); }

std::string gStateDir;
std::string gCoordSock;
std::string gCoordLock;
std::string gCoordBin;

void msleep(int ms) { struct timespec ts{ ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, nullptr); }

std::string selfDir()
{
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0; std::string p(buf);
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

// O3 / spike §3.1: connect to the coordinator, lazily spawning exactly one via
// an O_EXCL-lockfile + bind election. The loser of the race connects to the
// winner; a stale socket is reclaimed by the coordinator's unlink-before-bind.
int ensureCoordinator()
{
    for (int attempt = 0; attempt < 500; ++attempt) {
        int fd = connectSocket(gCoordSock);
        if (fd >= 0) return fd;

        // No coordinator reachable. Try to win the election.
        int lockfd = ::open(gCoordLock.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (lockfd >= 0) {                               // we are the spawner
            dbg("won election; spawning coordinator");
            pid_t pid = ::fork();
            if (pid == 0) {
                ::setsid();
                // argv[0] = full path so `pgrep -f "<bindir>/coordinator <dir>"`
                // (used by the tests/teardown) reliably matches the process.
                execlp(gCoordBin.c_str(), gCoordBin.c_str(), gStateDir.c_str(), (char *) nullptr);
                std::fprintf(stderr, "exec coordinator failed: %s\n", strerror(errno));
                _exit(127);
            }
            for (int i = 0; i < 300; ++i) {              // wait for the socket
                int c = connectSocket(gCoordSock);
                if (c >= 0) { ::unlink(gCoordLock.c_str()); ::close(lockfd); return c; }
                msleep(10);
            }
            ::unlink(gCoordLock.c_str()); ::close(lockfd);
        } else {
            // Someone else is spawning (EEXIST) -- back off and retry connect.
            // Reclaim an abandoned lock if it is clearly stale.
            struct stat st;
            if (errno == EEXIST && ::stat(gCoordLock.c_str(), &st) == 0
                && ::time(nullptr) - st.st_mtime > 30)
                ::unlink(gCoordLock.c_str());
            msleep(10);
        }
    }
    return -1;
}

// ---- writing client-facing records (the public-wire stand-in, §5.1) --------

bool sendLog(int clientFd, bool replayed, const std::string & bytes)
{
    BufWriter w; w.u8(uint8_t(CRec::Log)); w.u8(replayed ? 1 : 0); w.str(bytes);
    return writeAllBlocking(clientFd, frame(w.buf));
}
void sendResult(int clientFd, bool ok, uint32_t code, bool dedup, const std::string & logRef)
{
    BufWriter w; w.u8(uint8_t(CRec::Result)); w.u8(ok ? 1 : 0); w.u32(code); w.u8(dedup ? 1 : 0); w.str(logRef);
    writeAllBlocking(clientFd, frame(w.buf));
}
void sendDenied(int clientFd)
{
    BufWriter w; w.u8(uint8_t(CRec::Denied));
    writeAllBlocking(clientFd, frame(w.buf));
}

// O2 / §5.1: coordinator unreachable mid-build. Fall back to a local build,
// coalesced across concurrent fallers-back by a flock on a per-key lockfile --
// the prototype's stand-in for output PathLocks (§1.3). The first to get the
// lock runs the builder (counter++ once); the rest see the done-marker and skip.
// This is what makes A-crash's "exactly one rebuild" true.
void fallbackLocalBuild(int clientFd, const ClientRequest & q)
{
    dbg("FALLBACK to local build for " + q.buildKey);
    std::string lockPath = gStateDir + "/fallback-" + std::to_string(std::hash<std::string>{}(q.buildKey)) + ".lock";
    std::string donePath = lockPath + ".done";
    int lockfd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
    if (lockfd >= 0) ::flock(lockfd, LOCK_EX);

    struct stat st;
    if (::stat(donePath.c_str(), &st) == 0) {            // already built under the lock
        sendLog(clientFd, false, "[fallback] build already completed by a peer (PathLock coalesced)\n");
        sendResult(clientFd, true, 0, /*dedup=*/true, "");
    } else {
        std::string script = selfDir() + "/slow-builder.sh";
        std::string cmd = "/bin/sh '" + script + "' '" + q.counterFile + "' '" + q.buildKey
            + "' " + std::to_string(q.nLines) + " " + std::to_string(q.sleepMs);
        FILE * p = ::popen(cmd.c_str(), "r");
        bool ok = false;
        if (p) {
            char buf[4096]; size_t n;
            while ((n = ::fread(buf, 1, sizeof(buf), p)) > 0) sendLog(clientFd, false, std::string(buf, n));
            ok = (pclose(p) == 0);
        }
        if (ok) { int d = ::open(donePath.c_str(), O_CREAT | O_WRONLY, 0600); if (d >= 0) ::close(d); }
        sendResult(clientFd, ok, ok ? 0 : 1, false, "");
    }
    if (lockfd >= 0) { ::flock(lockfd, LOCK_UN); ::close(lockfd); }
}

// ---- the relay child: one connection, one build request (§3.4) -------------

void handleConnection(int clientFd)
{
    auto reqBody = readFrameBlocking(clientFd);
    if (!reqBody) return;
    ClientRequest q = ClientRequest::decode(*reqBody);
    dbg("request key=" + q.buildKey + " uid=" + std::to_string(q.uid) + " behavior=" + std::to_string(q.behavior));

    setSockBuf(clientFd);   // §3.6 test knob (relay -> client direction)
    int coord = ensureCoordinator();
    if (coord < 0) { fallbackLocalBuild(clientFd, q); return; }
    setSockBuf(coord);      // §3.6 test knob (coordinator -> relay direction)

    // START_OR_ATTACH (§3.3)
    {
        BufWriter w; w.u8(uint8_t(Op::StartOrAttach));
        w.str(q.buildKey); w.str(q.drvForBuild); w.u32(q.uid); w.u8(q.trusted);
        w.u8(q.replayWanted); w.u8(q.explicitRoot); w.str(q.counterFile); w.u32(q.nLines); w.u32(q.sleepMs);
        w.u8(q.ca); w.str(q.unresolvedDrv); w.str(q.resolvedDrv); w.u32(q.resolveMs);
        writeAllBlocking(coord, frame(w.buf));
    }
    auto replyBody = readFrameBlocking(coord);
    if (!replyBody) { ::close(coord); fallbackLocalBuild(clientFd, q); return; }  // coordinator vanished
    BufReader rr{*replyBody};
    if (Msg(rr.u8()) != Msg::StartReply) { ::close(coord); return; }
    Status status = Status(rr.u8());
    uint64_t subId = rr.u64();
    bool dedup = rr.u8() != 0;
    if (status == Status::Denied) { dbg("coordinator DENIED"); sendDenied(clientFd); ::close(coord); return; }

    // SUBSCRIBE -- begin the frame stream
    { BufWriter w; w.u8(uint8_t(Op::Subscribe)); w.u64(subId); writeAllBlocking(coord, frame(w.buf)); }

    // Relay loop: coordinator FRAMEs -> client; client HUP -> UNSUBSCRIBE (§3.4).
    std::string in;
    char tmp[8192];
    bool sentFirstLog = false;
    for (;;) {
        pollfd pfds[2] = { { coord, POLLIN, 0 }, { clientFd, POLLIN, 0 } };
        ::poll(pfds, 2, -1);

        // Client gone? Inverted MonitorFdHup: detach, do NOT kill the build.
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char d[256]; ssize_t n = ::read(clientFd, d, sizeof(d));
            if (n <= 0) {
                Op op = (q.behavior == 2) ? Op::CancelHint : Op::Unsubscribe;
                BufWriter w; w.u8(uint8_t(op)); w.u64(subId); writeAllBlocking(coord, frame(w.buf));
                dbg("client gone -> sent " + std::string(op == Op::CancelHint ? "CANCEL_HINT" : "UNSUBSCRIBE"));
                break;
            }
        }

        if (pfds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = ::read(coord, tmp, sizeof(tmp));
            if (n <= 0) {                                  // coordinator crashed (§5.1)
                dbg("coordinator EOF -> fallback");
                ::close(coord);
                fallbackLocalBuild(clientFd, q);
                return;
            }
            in.append(tmp, size_t(n));
            while (auto body = takeFrame(in)) {
                BufReader r{*body};
                switch (Msg(r.u8())) {
                case Msg::Frame: {
                    bool replayed = r.u8() != 0;
                    std::string bytes = r.str();
                    sendLog(clientFd, replayed, bytes);
                    if (!sentFirstLog && q.behavior == 2) {
                        // disconnect-after-first-log test driver: simulate the
                        // client dropping right after seeing output.
                        sentFirstLog = true;
                    }
                    break;
                }
                case Msg::BuildResult: {
                    bool ok = r.u8() != 0; uint32_t code = r.u32(); bool d = r.u8() != 0; std::string logRef = r.str();
                    sendResult(clientFd, ok, code, d || dedup, logRef);
                    ::close(coord);
                    return;
                }
                case Msg::AttachState: (void) r.u8(); break;
                default: break;
                }
            }
        }
    }
    ::close(coord);
}

int gListenFd = -1;
void onChld(int) { while (::waitpid(-1, nullptr, WNOHANG) > 0) {} }

} // namespace

int main(int argc, char ** argv)
{
    gStateDir  = (argc > 1) ? argv[1] : (::getenv("WSA_STATE_DIR") ?: "/tmp/wsa");
    gCoordSock = gStateDir + "/coordinator.socket";
    gCoordLock = gStateDir + "/coordinator.lock";
    gCoordBin  = selfDir() + "/coordinator";
    std::string daemonSock = gStateDir + "/daemon.socket";

    ::mkdir(gStateDir.c_str(), 0750);
    signal(SIGCHLD, onChld);
    signal(SIGPIPE, SIG_IGN);

    try {
        gListenFd = makeListenSocket(daemonSock);
    } catch (const std::exception & e) {
        std::fprintf(stderr, "[reldaemon] %s\n", e.what());
        return 1;
    }
    dbg("listening on " + daemonSock);

    for (;;) {
        int clientFd = ::accept(gListenFd, nullptr, nullptr);
        if (clientFd < 0) { if (errno == EINTR) continue; break; }
        pid_t pid = ::fork();                              // fork-per-connection (§1.1)
        if (pid == 0) {
            ::close(gListenFd);
            // Drop the parent's SIGCHLD reaper: this child uses popen()/pclose()
            // (and may fork a coordinator), which need to wait on their own
            // children without the parent's handler stealing the exit status.
            signal(SIGCHLD, SIG_DFL);
            try { handleConnection(clientFd); } catch (const std::exception & e) { dbg(std::string("error: ") + e.what()); }
            ::close(clientFd);
            _exit(0);
        }
        ::close(clientFd);
    }
    return 0;
}
