// Workstream A prototype — test client (the `nix build` stand-in).
//
// THROW-AWAY CODE (see proto.hh). Connects to the relay daemon, issues one
// build request, and consumes the client-facing record stream (CRec). It only
// ever sees CRec::Log / CRec::Result / CRec::Denied -- the public-wire stand-in
// (§5.1); it cannot tell whether the build was coordinator-relayed or local.
//
// stdout : the reconstructed log bytes, replayed prefix then live tail, in order
//          (so "client2 stdout == client1 stdout" proves replay+live == full).
// stderr : machine-readable summary lines the test harness greps:
//            DEDUP=0|1   RESULT ok=.. code=..   DENIED   REPLAYED=N LIVE=M
//
// Behaviors (mirror ClientRequest::behavior):
//   0 normal           — read to completion
//   1 slow-reader      — read one record, sleep, exit (triggers §3.6 demote)
//   2 disconnect-first — read one log record then drop (refcount-cancel tests)

#include "proto.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <getopt.h>
#include <time.h>
#include <unistd.h>

using namespace wsa;

static void msleep(int ms) { struct timespec ts{ ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, nullptr); }

// slow-reader sleep, overridable for the backpressure test.
static size_t envSizeMs()
{
    if (const char * v = ::getenv("WSA_CLIENT_SLEEP_MS")) { try { return std::stoul(v); } catch (...) {} }
    return 1500;
}

int main(int argc, char ** argv)
{
    ClientRequest q;
    q.nLines = 6; q.sleepMs = 300;
    std::string stateDir = ::getenv("WSA_STATE_DIR") ?: "/tmp/wsa";

    static option opts[] = {
        {"key", required_argument, 0, 'k'},
        {"drv", required_argument, 0, 'd'},
        {"counter", required_argument, 0, 'c'},
        {"lines", required_argument, 0, 'n'},
        {"sleep-ms", required_argument, 0, 's'},
        {"uid", required_argument, 0, 'u'},
        {"trusted", required_argument, 0, 't'},
        {"replay", required_argument, 0, 'r'},
        {"root", required_argument, 0, 'R'},
        {"behavior", required_argument, 0, 'b'},
        {"state", required_argument, 0, 'S'},
        {"ca", no_argument, 0, 'A'},
        {"unresolved", required_argument, 0, 'U'},
        {"resolved", required_argument, 0, 'V'},
        {"resolve-ms", required_argument, 0, 'M'},
        {"assert-key", required_argument, 0, 'K'},
        {0, 0, 0, 0}
    };
    int ch;
    bool assertGiven = false;
    while ((ch = getopt_long(argc, argv, "k:d:c:n:s:u:t:r:R:b:S:AU:V:M:K:", opts, nullptr)) != -1) {
        switch (ch) {
        case 'k': q.buildKey = optarg; break;
        case 'd': q.drvForBuild = optarg; break;
        case 'c': q.counterFile = optarg; break;
        case 'n': q.nLines = std::stoul(optarg); break;
        case 's': q.sleepMs = std::stoul(optarg); break;
        case 'u': q.uid = std::stoul(optarg); break;
        case 't': q.trusted = std::stoul(optarg); break;
        case 'r': q.replayWanted = std::stoul(optarg); break;
        case 'R': q.explicitRoot = std::stoul(optarg); break;
        case 'b': q.behavior = std::stoul(optarg); break;
        case 'S': stateDir = optarg; break;
        case 'A': q.ca = 1; break;
        case 'U': q.unresolvedDrv = optarg; break;
        case 'V': q.resolvedDrv = optarg; break;
        case 'M': q.resolveMs = std::stoul(optarg); break;
        case 'K': q.buildKey = optarg; assertGiven = true; break;  // T3 spoof override
        default: return 2;
        }
    }
    if (q.ca) {
        // CA: authorized against the unresolved drv; the honest asserted key is
        // the resolved drv (the coordinator recomputes and checks it, T3).
        if (q.drvForBuild.empty()) q.drvForBuild = q.unresolvedDrv;
        if (!assertGiven) q.buildKey = q.resolvedDrv;
        if (q.counterFile.empty()) q.counterFile = stateDir + "/counter-" + q.resolvedDrv;
    } else {
        if (q.drvForBuild.empty()) q.drvForBuild = q.buildKey;
        if (q.counterFile.empty()) q.counterFile = stateDir + "/counter-" + q.buildKey;
    }
    if (!q.uid) q.uid = ::getuid();

    int fd = connectSocket(stateDir + "/daemon.socket");
    if (fd < 0) { std::fprintf(stderr, "connect daemon: %s\n", strerror(errno)); return 1; }
    setSockBuf(fd);   // §3.6 test knob (client recv side)

    if (!writeAllBlocking(fd, frame(q.encode()))) { std::fprintf(stderr, "send request failed\n"); return 1; }

    std::string in;
    char tmp[8192];
    size_t replayedBytes = 0, liveBytes = 0;
    int records = 0;
    for (;;) {
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; break; }
        in.append(tmp, size_t(n));
        bool done = false;
        while (auto body = takeFrame(in)) {
            BufReader r{*body};
            switch (CRec(r.u8())) {
            case CRec::Log: {
                bool replayed = r.u8() != 0;
                std::string bytes = r.str();
                ::fwrite(bytes.data(), 1, bytes.size(), stdout);
                if (replayed) replayedBytes += bytes.size(); else liveBytes += bytes.size();
                ++records;
                if (q.behavior == 1 && records == 1) {        // slow-reader
                    ::fflush(stdout);
                    msleep(int(envSizeMs()));
                    std::fprintf(stderr, "REPLAYED=%zu LIVE=%zu\n", replayedBytes, liveBytes);
                    return 0;
                }
                if (q.behavior == 2 && records == 1) {        // disconnect-after-first
                    ::fflush(stdout);
                    std::fprintf(stderr, "DISCONNECT_AFTER_FIRST\n");
                    return 0;
                }
                break;
            }
            case CRec::Result: {
                bool ok = r.u8() != 0; uint32_t code = r.u32(); bool dedup = r.u8() != 0; std::string logRef = r.str();
                std::fprintf(stderr, "DEDUP=%d\nRESULT ok=%d code=%u log=%s\n", dedup ? 1 : 0, ok ? 1 : 0, code, logRef.c_str());
                done = true;
                break;
            }
            case CRec::Denied:
                std::fprintf(stderr, "DENIED\n");
                done = true;
                break;
            }
            if (done) break;
        }
        if (done) break;
    }
    ::fflush(stdout);
    std::fprintf(stderr, "REPLAYED=%zu LIVE=%zu\n", replayedBytes, liveBytes);
    return 0;
}
