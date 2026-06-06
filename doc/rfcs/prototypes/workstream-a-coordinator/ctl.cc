// Workstream A prototype — direct control-socket probe.
//
// THROW-AWAY CODE (see proto.hh). Talks the §3.2 control protocol straight to
// the coordinator (bypassing the relay) to exercise the two properties that are
// about the control socket itself rather than the build relay:
//
//   * A-sockauth (§3.7.1): if the coordinator's expected daemon uid does not
//     match this process's uid, the peer-cred check refuses the connection
//     before a byte of sessionAuth is read -> we observe an immediate EOF.
//   * QUERY_ACTIVE filtering (§3.7.3): an untrusted caller sees only builds it
//     is itself authorized for; a trusted caller sees all.
//
// Output (stderr): REFUSED | CONNECT_FAILED | ACTIVE count=N [key ...]

#include "proto.hh"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace wsa;

int main(int argc, char ** argv)
{
    std::string stateDir = ::getenv("WSA_STATE_DIR") ?: "/tmp/wsa";
    uint32_t uid = ::getuid();
    uint8_t trusted = 1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--untrusted") trusted = 0;
        else if (a == "--state" && i + 1 < argc) stateDir = argv[++i];
        else if (a == "--uid" && i + 1 < argc) uid = std::stoul(argv[++i]);
    }

    int fd = connectSocket(stateDir + "/coordinator.socket");
    if (fd < 0) { std::fprintf(stderr, "CONNECT_FAILED\n"); return 1; }

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
