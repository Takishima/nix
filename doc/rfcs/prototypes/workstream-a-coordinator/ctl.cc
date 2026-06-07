// Workstream A/B prototype — direct control-socket probe.
//
// THROW-AWAY CODE (see proto.hh). Talks the §3.2 control protocol straight to
// the coordinator (bypassing the relay) to exercise properties about the control
// socket itself:
//
//   default mode (QUERY_ACTIVE):
//     * A-sockauth (§3.7.1): a uid mismatch is refused at the peer-cred check
//       before any sessionAuth is read -> immediate EOF (REFUSED).
//     * QUERY_ACTIVE filtering (§3.7.3): untrusted sees only its own builds.
//
//   --start mode (START_OR_ATTACH):
//     * T1 existence oracle (§3.7 / Workstream B): an unauthorized START for a
//       resolved key that IS vs is NOT in flight returns a byte-identical denial
//       with no timing difference (authorize() runs before the registry lookup).
//     * T3 asserted-key spoof: a child asserting a key != the drv it sends is
//       rejected.
//
// Output (stderr): REFUSED | CONNECT_FAILED | ACTIVE count=N ... |
//                  START ok dedup=N elapsed_us=M | START denied elapsed_us=M

#include "proto.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <time.h>

using namespace wsa;

static long nowUs()
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return long(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

int main(int argc, char ** argv)
{
    std::string stateDir = ::getenv("WSA_STATE_DIR") ?: "/tmp/wsa";
    uint32_t uid = ::getuid();
    uint8_t trusted = 1;
    bool doStart = false;
    ClientRequest q;
    q.nLines = 4; q.sleepMs = 100;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--untrusted") trusted = 0;
        else if (a == "--state") stateDir = next();
        else if (a == "--uid") uid = std::stoul(next());
        else if (a == "--start") doStart = true;
        else if (a == "--key") q.buildKey = next();
        else if (a == "--drv") q.drvForBuild = next();
        else if (a == "--ca") q.ca = 1;
        else if (a == "--unresolved") q.unresolvedDrv = next();
        else if (a == "--resolved") q.resolvedDrv = next();
        else if (a == "--resolve-ms") q.resolveMs = std::stoul(next());
        else if (a == "--counter") q.counterFile = next();
        else if (a == "--lines") q.nLines = std::stoul(next());
        else if (a == "--sleep-ms") q.sleepMs = std::stoul(next());
    }
    q.uid = uid; q.trusted = trusted;

    int fd = connectSocket(stateDir + "/coordinator.socket");
    if (fd < 0) { std::fprintf(stderr, "CONNECT_FAILED\n"); return 1; }

    if (doStart) {
        BufWriter w; w.u8(uint8_t(Op::StartOrAttach));
        w.str(q.buildKey); w.str(q.drvForBuild); w.u32(q.uid); w.u8(q.trusted);
        w.u8(q.replayWanted); w.u8(q.explicitRoot); w.str(q.counterFile); w.u32(q.nLines); w.u32(q.sleepMs);
        w.u8(q.ca); w.str(q.unresolvedDrv); w.str(q.resolvedDrv); w.u32(q.resolveMs);
        w.u32(q.timeoutMs); w.u8(q.keepFailed); w.u32(q.failAt); w.u32(q.failCode);  // keep in sync with the coordinator parser

        long t0 = nowUs();
        if (!writeAllBlocking(fd, frame(w.buf))) { std::fprintf(stderr, "REFUSED\n"); return 1; }
        auto body = readFrameBlocking(fd);
        long us = nowUs() - t0;
        if (!body) { std::fprintf(stderr, "REFUSED\n"); return 1; }
        BufReader r{*body};
        if (Msg(r.u8()) != Msg::StartReply) { std::fprintf(stderr, "BADREPLY\n"); return 1; }
        Status st = Status(r.u8());
        uint64_t subId = r.u64();
        bool dedup = r.u8() != 0;
        if (st == Status::Ok) {
            std::fprintf(stderr, "START ok dedup=%d elapsed_us=%ld\n", dedup ? 1 : 0, us);
            BufWriter u; u.u8(uint8_t(Op::Unsubscribe)); u.u64(subId);  // clean up
            writeAllBlocking(fd, frame(u.buf));
        } else {
            std::fprintf(stderr, "START denied elapsed_us=%ld\n", us);
        }
        return 0;
    }

    BufWriter w; w.u8(uint8_t(Op::QueryActive)); w.u32(uid); w.u8(trusted);
    if (!writeAllBlocking(fd, frame(w.buf))) { std::fprintf(stderr, "REFUSED\n"); return 1; }
    auto body = readFrameBlocking(fd);
    if (!body) { std::fprintf(stderr, "REFUSED\n"); return 1; }   // peer-cred refusal => EOF
    BufReader r{*body};
    if (Msg(r.u8()) != Msg::ActiveList) { std::fprintf(stderr, "BADREPLY\n"); return 1; }
    uint32_t count = r.u32();
    std::string out = "ACTIVE count=" + std::to_string(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string key = r.str(); uint32_t subs = r.u32(); uint32_t bytes = r.u32();
        out += " " + key + "(subs=" + std::to_string(subs) + ",bytes=" + std::to_string(bytes) + ")";
    }
    std::fprintf(stderr, "%s\n", out.c_str());
    return 0;
}
