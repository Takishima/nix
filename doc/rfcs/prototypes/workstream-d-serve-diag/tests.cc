// Workstream D prototype — serve diagnostic core golden & characterisation tests (D2).
//
// THROW-AWAY CODE. Encodes the guarding tests of decisions B3 §4 (RFC §9
// "Protocol characterisation tests"):
//   * golden serializations of BuildResult at 2.3 / 2.6 / 2.8 / 2.9;
//   * the 2.8-reads-2.9-bytes test (2.8 reader consumes exactly the 2.8 fields;
//     a negotiated-down peer never emits the 2.9 tail);
//   * a QueryBuildLog round-trip;
//   * a "nix log over serve" functional check (Gap A / §4.5);
//   * the full back-compat matrix, both directions, via the min() handshake.
//
// Run with `--dump` to print the golden hex (used to (re)generate the constants
// below); run with no args to execute the test suite.

#include "serve.hh"

#include <cstdio>
#include <cstring>
#include <string>

using namespace serve;

static int gPass = 0, gFail = 0;
static void ok(const std::string & m)  { ++gPass; std::printf("  PASS: %s\n", m.c_str()); }
static void bad(const std::string & m) { ++gFail; std::printf("  FAIL: %s\n", m.c_str()); }
static void check(bool c, const std::string & m) { c ? ok(m) : bad(m); }

// A fixed, fully-populated sample so the goldens are deterministic.
static BuildResult sample()
{
    BuildResult r;
    r.status = Status::PermanentFailure;
    r.errorMsg = "builder failed";
    r.timesBuilt = 2;
    r.isNonDeterministic = 1;
    r.startTime = 1000;
    r.stopTime = 1042;
    r.builtOutputs = { {"out", "/nix/store/aaaaaaaaaaaaaaaa-foo"} };
    r.logRef = "/nix/store/bbbbbbbbbbbbbbbb-foo.drv";
    r.failurePhase = "build";
    r.exitCode = 1;
    r.logTail = "error: command failed\n";
    r.builderId = "builder-7";
    r.deduplicated = 1;
    return r;
}

static std::string ser(Version v, const BuildResult & r)
{
    Sink s; write(s, v, r); return s.buf;
}

// ---- golden bytes (generated via --dump; characterisation guard) -----------
// If the layout changes, these must change too -- which is exactly the review
// signal a serve diagnostic-core freeze needs.
static const char * GOLD_2_3 = "04000000000000000e000000000000006275696c646572206661696c6564000002000000000000000100000000000000e8030000000000001204000000000000";
static const char * GOLD_2_6 = "04000000000000000e000000000000006275696c646572206661696c6564000002000000000000000100000000000000e803000000000000120400000000000001000000000000004b000000000000007368613235363a30303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030216f7574000000000080000000000000007b226964223a227368613235363a30303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030216f7574222c226f757450617468223a222f6e69782f73746f72652f616161616161616161616161616161612d666f6f227d";
static const char * GOLD_2_8 = "04000000000000000e000000000000006275696c646572206661696c6564000002000000000000000100000000000000e8030000000000001204000000000000010000000000000003000000000000006f757400000000001f000000000000002f6e69782f73746f72652f616161616161616161616161616161612d666f6f00";
static const char * GOLD_2_9 = "04000000000000000e000000000000006275696c646572206661696c6564000002000000000000000100000000000000e8030000000000001204000000000000010000000000000003000000000000006f757400000000001f000000000000002f6e69782f73746f72652f616161616161616161616161616161612d666f6f0023000000000000002f6e69782f73746f72652f626262626262626262626262626262622d666f6f2e647276000000000005000000000000006275696c64000000010000000000000016000000000000006572726f723a20636f6d6d616e64206661696c65640a0000";

static void dump()
{
    auto r = sample();
    std::printf("GOLD_2_3 %s\n", toHex(ser(V2_3, r)).c_str());
    std::printf("GOLD_2_6 %s\n", toHex(ser(V2_6, r)).c_str());
    std::printf("GOLD_2_8 %s\n", toHex(ser(V2_8, r)).c_str());
    std::printf("GOLD_2_9 %s\n", toHex(ser(V2_9, r)).c_str());
}

// round-trip at one version: write then read at the same negotiated version.
static bool roundtrips(Version v)
{
    auto r = sample();
    std::string bytes = ser(v, r);          // named: Source holds a view into it
    Source src{bytes};
    BuildResult got = read(src, v);
    if (!src.eof()) return false;                       // consumed exactly
    // fields present at this version must match
    bool eq = got.status == r.status && got.errorMsg == r.errorMsg;
    if (v >= V2_3) eq = eq && got.timesBuilt == r.timesBuilt && got.isNonDeterministic == r.isNonDeterministic
                          && got.startTime == r.startTime && got.stopTime == r.stopTime;
    if (v >= V2_6) eq = eq && got.builtOutputs == r.builtOutputs;
    if (v >= V2_9) eq = eq && got.logRef == r.logRef && got.failurePhase == r.failurePhase
                          && got.exitCode == r.exitCode && got.logTail == r.logTail;
    return eq;
}

int main(int argc, char ** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) { dump(); return 0; }

    std::printf("[serve diagnostic-core] golden & characterisation tests (Blocker 3)\n");
    auto r = sample();

    // 1. Round-trip at every version on the ladder.
    check(roundtrips(V2_3), "round-trip BuildResult @ 2.3");
    check(roundtrips(V2_6), "round-trip BuildResult @ 2.6");
    check(roundtrips(V2_8), "round-trip BuildResult @ 2.8");
    check(roundtrips(V2_9), "round-trip BuildResult @ 2.9");

    // 2. Golden bytes (characterisation: catch any accidental layout drift).
    check(toHex(ser(V2_3, r)) == GOLD_2_3, "golden bytes stable @ 2.3");
    check(toHex(ser(V2_6, r)) == GOLD_2_6, "golden bytes stable @ 2.6");
    check(toHex(ser(V2_8, r)) == GOLD_2_8, "golden bytes stable @ 2.8");
    check(toHex(ser(V2_9, r)) == GOLD_2_9, "golden bytes stable @ 2.9");

    // 3. The 2.9 layout is purely ADDITIVE: the 2.8 bytes are a prefix of the 2.9
    //    bytes (no existing field changes meaning).
    {
        std::string b28 = ser(V2_8, r), b30 = ser(V2_9, r);
        check(b30.size() > b28.size() && b30.compare(0, b28.size(), b28) == 0,
              "2.9 bytes == 2.8 bytes + appended diagnostic-core tail");
    }

    // 4. 2.8-reads-2.9-bytes: a 2.8 reader consumes EXACTLY the 2.8 fields and
    //    stops before the 2.9 tail; the leftover bytes are exactly that tail.
    {
        std::string b30 = ser(V2_9, r);
        Source src{b30};
        BuildResult got28 = read(src, V2_8);            // read with the 2.8 reader
        size_t consumed = src.pos;
        std::string tail = b30.substr(consumed);
        // what the 2.8 reader got equals a pure-2.8 serialization+read
        bool same28 = got28.builtOutputs == r.builtOutputs && got28.logRef.empty() && got28.logTail.empty();
        check(same28, "2.8 reader consumes exactly the 2.8 fields, no diagnostic-core fields");
        check(consumed == ser(V2_8, r).size(), "2.8 reader stops at the 2.8 boundary");
        // the leftover tail is exactly the standalone-serialized 2.9 core
        Sink coreOnly;
        coreOnly.putString(r.logRef); coreOnly.putString(r.failurePhase);
        coreOnly.putInt(uint64_t(r.exitCode)); coreOnly.putString(r.logTail);
        check(tail == coreOnly.buf, "leftover bytes are exactly the diagnostic-core tail");
    }

    // 5. Negotiated-down peer never EMITS the tail. With min() handshake, a
    //    2.9-capable Nix talking to a 2.8 Hydra serializes at 2.8 → no tail.
    {
        Version neg = negotiate(V2_9, V2_8);            // = 2.8
        check(neg == V2_8, "min() handshake: 2.9 vs 2.8 -> 2.8");
        std::string b = ser(neg, r);
        check(b == ser(V2_8, r), "negotiated-down (2.8) emits no diagnostic-core tail");
        check(!supportsQueryBuildLog(neg), "QueryBuildLog not offered at negotiated 2.8");
    }

    // 6. Back-compat matrix (decisions B3 §3), both directions — at the
    // COMPATIBLE wire version V2_9 (minor bump within major 2).
    check(negotiate(V2_8, V2_9) == V2_8, "old Hydra (2.8) <-> new Nix (2.9): negotiate 2.8");
    check(negotiate(V2_9, V2_8) == V2_8, "new Hydra (2.9) <-> old Nix (2.8): negotiate 2.8");
    check(negotiate(V2_9, V2_9) == V2_9 && supportsQueryBuildLog(negotiate(V2_9, V2_9)),
          "new <-> new: negotiate 2.9, full diagnostic core active");

    // 6b. The compatibility correction (decisions Blocker 3): the client handshake
    // guard runs BEFORE min(), so a MAJOR bump is not protected by additivity.
    // This is the regression the earlier (3<<8|0) plan would have shipped.
    {
        // old client reaching an upgraded builder:
        check(!clientAcceptsServer(V3_0),
              "REGRESSION GUARD: old client (major==2) REJECTS a {3,0} builder before min()");
        check(clientAcceptsServer(V2_9),
              "old client ACCEPTS a {2,9} builder (same major)");
        // and once accepted, min() degrades it to today's exchange:
        check(negotiate(V2_8, V2_9) == V2_8 && !supportsQueryBuildLog(negotiate(V2_8, V2_9)),
              "accepted {2,9} builder negotiates {2,8} for the old client — no new fields/op");
        // sanity: the supported floor is still enforced.
        check(!clientAcceptsServer(Version{2, 4}),
              "client still rejects a below-floor {2,4} server");
    }

    // 7. QueryBuildLog round-trip + "nix log over serve" (Gap A / §4.5).
    {
        LogStore store;
        store.logs[r.logRef] = "configuring...\nbuilding...\nerror: command failed\n";
        // client (negotiated 2.9) sends QueryBuildLog{logRef}; server serves it.
        Sink req; writeQueryBuildLogRequest(req, r.logRef);
        Source reqSrc{req.buf};
        std::string got = serveQueryBuildLog(reqSrc, store);
        check(got == store.logs[r.logRef], "QueryBuildLog returns the real persisted log (nix log works)");
        // a missing log yields empty (not a crash / not 'unsupported')
        Sink req2; writeQueryBuildLogRequest(req2, "/nix/store/zzzz-missing.drv");
        Source reqSrc2{req2.buf};
        check(serveQueryBuildLog(reqSrc2, store).empty(), "QueryBuildLog for an unknown drv is empty, not an error");
    }

    // 8. Deferred set stays unstable: the unstable serialization is the frozen
    //    2.9 prefix plus builderId/deduplicated -- the frozen layout is undisturbed.
    {
        std::string b30 = ser(V2_9, r), bU = ser(Vunstable, r);
        check(bU.size() > b30.size() && bU.compare(0, b30.size(), b30) == 0,
              "unstable builderId/deduplicated append AFTER the frozen 2.9 core");
    }

    std::printf("  -- %d passed, %d failed --\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
