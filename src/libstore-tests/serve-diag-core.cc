// Golden / back-compat characterisation of the candidate serve 2.9 "diagnostic
// core" layout: logRef, failurePhase, exitCode, logTail appended after the 2.8
// `builtOutputs` block under a `>= {2,9}` guard, plus QueryBuildLog as
// Command = 10.
//
// The production serializers (src/libstore/{serve,worker}-protocol.cc) carry
// this layout, but only behind the unstable gate and WITHOUT bumping
// SERVE_PROTOCOL_VERSION — i.e. it is not yet a back-compat promise. This file
// is a self-contained model of the same version-gated byte ladder, so the
// goldens fail on any accidental drift while the layout is still unstable.
// Once the version is bumped, the production fixtures take over and this model
// is deleted.

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "nix/store/serve-protocol.hh"

// NOTE: this lives in its own namespace, *not* `nix`, on purpose. The production
// `serve-protocol.hh` transitively defines `nix::Sink`, `nix::Source`, and
// `nix::BuildResult`; the self-contained model below intentionally reuses those
// names, so being inside `nix` would make every unqualified use ambiguous. Only
// `SERVE_PROTOCOL_VERSION` (a macro) is borrowed from the real header.
namespace serve_diag_core_test {

// If someone bumps the real version, this assert fires and forces them to
// decide deliberately that the 2.9 layout is final (and to retarget these
// goldens at the production serializer).
static_assert(
    SERVE_PROTOCOL_VERSION == (2 << 8 | 8),
    "the serve diagnostic-core layout is not frozen; SERVE_PROTOCOL_VERSION must stay 2.8 "
    "until it is");

namespace {

// ---- candidate serve 2.9 model ---------------------------------------------

struct Version
{
    uint8_t major = 0, minor = 0;
    bool operator>=(const Version & o) const
    {
        return major != o.major ? major > o.major : minor >= o.minor;
    }
    bool operator==(const Version & o) const { return major == o.major && minor == o.minor; }
};

// The handshake takes the minimum of the two peers' versions (back-compat
// matrix): both sides then speak exactly that version.
Version negotiate(Version a, Version b)
{
    return (a >= b) ? b : a;
}

constexpr Version V2_3{2, 3};
constexpr Version V2_6{2, 6};
constexpr Version V2_8{2, 8}; // current SERVE_PROTOCOL_VERSION
constexpr Version V2_9{2, 9}; // diagnostic core — the COMPATIBLE wire version (minor bump)
constexpr Version V3_0{3, 0}; // a major bump — REJECTED by the client handshake (see below)
constexpr Version Vunstable{2, 99}; // deferred builderId/deduplicated, NOT frozen

// The serve *client* handshake guard, faithful to serve-protocol-connection.cc:
//   if (remoteVersion.major != 2 || remoteVersion < {2,5}) throw "unsupported …";
// This runs BEFORE min(), so a {3,0} builder breaks every already-deployed
// client — which is exactly why the diagnostic core ships as {2,9}, not {3,0}.
bool clientAcceptsServer(Version remoteServerVersion)
{
    return remoteServerVersion.major == 2 && remoteServerVersion >= Version{2, 5};
}

enum class Command : uint64_t {
    AddToStoreNar = 9,
    QueryBuildLog = 10, // NEW; guarded by >= {2,9}
};

bool supportsQueryBuildLog(Version negotiated)
{
    return negotiated >= V2_9;
}

// ---- wire primitives (mirror Nix's serialise.hh shape) ----------------------

struct Sink
{
    std::string buf;
    void putInt(uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            buf.push_back(char((v >> (i * 8)) & 0xff)); // little-endian
    }
    void putString(std::string_view s)
    {
        putInt(s.size());
        buf.append(s);
        while (buf.size() % 8 != 0)
            buf.push_back('\0'); // pad to a multiple of 8
    }
};

struct Source
{
    std::string_view s;
    size_t pos = 0;
    bool eof() const { return pos >= s.size(); }
    uint64_t getInt()
    {
        if (pos + 8 > s.size())
            throw std::runtime_error("short read (int)");
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= uint64_t(uint8_t(s[pos + i])) << (i * 8);
        pos += 8;
        return v;
    }
    std::string getString()
    {
        uint64_t n = getInt();
        if (pos + n > s.size())
            throw std::runtime_error("short read (string)");
        std::string out(s.substr(pos, n));
        pos += n;
        while (pos % 8 != 0)
            ++pos; // skip padding
        return out;
    }
};

// ---- the BuildResult model --------------------------------------------------

enum class Status : uint64_t { Built = 0, PermanentFailure = 4 };

struct BuildResult
{
    Status status = Status::Built;
    std::string errorMsg;

    // >= {2,3}
    uint64_t timesBuilt = 0;
    uint8_t isNonDeterministic = 0;
    uint64_t startTime = 0;
    uint64_t stopTime = 0;

    // >= {2,8} binary (>= {2,6} JSON-hack): outputName -> outPath
    std::map<std::string, std::string> builtOutputs;

    // >= {2,9} — the FROZEN diagnostic core
    std::string logRef;
    std::string failurePhase;
    int64_t exitCode = 0;
    std::string logTail;

    // unstable (>= {2,99}) — DEFERRED, layout NOT promised (builderId,
    // deduplicated, then failureClass and the memory fields)
    std::string builderId;
    uint8_t deduplicated = 0;
    uint64_t failureClass = 0;
    uint8_t killedForMemory = 0;
    uint64_t peakMemoryBytes = 0;
};

std::string dummyHash()
{
    return std::string("sha256:") + std::string(64, '0');
}

// write — mirrors serve-protocol.cc Serialise<BuildResult>::write
void write(Sink & to, Version v, const BuildResult & res)
{
    to.putInt(uint64_t(res.status));
    to.putString(res.errorMsg);

    if (v >= V2_3) {
        to.putInt(res.timesBuilt);
        to.putInt(res.isNonDeterministic);
        to.putInt(res.startTime);
        to.putInt(res.stopTime);
    }

    if (v >= V2_8) { // binary map
        to.putInt(res.builtOutputs.size());
        for (auto & [name, outPath] : res.builtOutputs) {
            to.putString(name);
            to.putString(outPath);
        }
    } else if (v >= V2_6) { // legacy JSON-hack StringMap
        to.putInt(res.builtOutputs.size());
        for (auto & [name, outPath] : res.builtOutputs) {
            std::string id = dummyHash() + "!" + name;
            std::string j = "{\"id\":\"" + id + "\",\"outPath\":\"" + outPath + "\"}";
            to.putString(id);
            to.putString(j);
        }
    }

    // Diagnostic core, appended AFTER builtOutputs, binary, gated >= {2,9}.
    if (v >= V2_9) {
        to.putString(res.logRef);
        to.putString(res.failurePhase);
        to.putInt(uint64_t(res.exitCode));
        to.putString(res.logTail);
    }

    // Deferred/unstable set — only on the explicitly-unstable version.
    if (v >= Vunstable) {
        to.putString(res.builderId);
        to.putInt(res.deduplicated);
        to.putInt(res.failureClass);
        to.putInt(res.killedForMemory);
        to.putInt(res.peakMemoryBytes);
    }
}

// read — mirrors serve-protocol.cc Serialise<BuildResult>::read
BuildResult read(Source & from, Version v)
{
    BuildResult res;
    res.status = Status(from.getInt());
    res.errorMsg = from.getString();

    if (v >= V2_3) {
        res.timesBuilt = from.getInt();
        res.isNonDeterministic = uint8_t(from.getInt());
        res.startTime = from.getInt();
        res.stopTime = from.getInt();
    }

    if (v >= V2_8) {
        uint64_t n = from.getInt();
        for (uint64_t i = 0; i < n; ++i) {
            auto name = from.getString();
            auto outPath = from.getString();
            res.builtOutputs[name] = outPath;
        }
    } else if (v >= V2_6) {
        uint64_t n = from.getInt();
        for (uint64_t i = 0; i < n; ++i) {
            std::string id = from.getString();
            std::string j = from.getString();
            size_t bang = id.find('!');
            std::string name = bang == std::string::npos ? id : id.substr(bang + 1);
            size_t k = j.find("\"outPath\":\"");
            std::string outPath;
            if (k != std::string::npos) {
                k += 11;
                outPath = j.substr(k, j.find('"', k) - k);
            }
            res.builtOutputs[name] = outPath;
        }
    }

    if (v >= V2_9) {
        res.logRef = from.getString();
        res.failurePhase = from.getString();
        res.exitCode = int64_t(from.getInt());
        res.logTail = from.getString();
    }

    if (v >= Vunstable) {
        res.builderId = from.getString();
        res.deduplicated = uint8_t(from.getInt());
        res.failureClass = from.getInt();
        res.killedForMemory = uint8_t(from.getInt());
        res.peakMemoryBytes = from.getInt();
    }

    return res;
}

std::string toHex(std::string_view b)
{
    static const char * h = "0123456789abcdef";
    std::string out;
    for (unsigned char c : b) {
        out.push_back(h[c >> 4]);
        out.push_back(h[c & 0xf]);
    }
    return out;
}

// ---- QueryBuildLog (Command = 10) -------------------------------------------

struct LogStore
{
    std::map<std::string, std::string> logs; // logRef -> log contents
    std::string get(const std::string & logRef) const
    {
        auto it = logs.find(logRef);
        return it == logs.end() ? std::string() : it->second;
    }
};

void writeQueryBuildLogRequest(Sink & to, const std::string & drvPath)
{
    to.putInt(uint64_t(Command::QueryBuildLog));
    to.putString(drvPath);
}

std::string serveQueryBuildLog(Source & from, const LogStore & store)
{
    auto cmd = Command(from.getInt());
    if (cmd != Command::QueryBuildLog)
        throw std::runtime_error("unexpected command");
    std::string drvPath = from.getString();
    return store.get(drvPath);
}

// A fixed, fully-populated sample so the goldens are deterministic.
BuildResult sample()
{
    BuildResult r;
    r.status = Status::PermanentFailure;
    r.errorMsg = "builder failed";
    r.timesBuilt = 2;
    r.isNonDeterministic = 1;
    r.startTime = 1000;
    r.stopTime = 1042;
    r.builtOutputs = {{"out", "/nix/store/aaaaaaaaaaaaaaaa-foo"}};
    r.logRef = "/nix/store/bbbbbbbbbbbbbbbb-foo.drv";
    r.failurePhase = "build";
    r.exitCode = 1;
    r.logTail = "error: command failed\n";
    r.builderId = "builder-7";
    r.deduplicated = 1;
    return r;
}

std::string ser(Version v, const BuildResult & r)
{
    Sink s;
    write(s, v, r);
    return s.buf;
}

// Golden bytes for the candidate layout: a characterisation guard against
// accidental drift while the layout is still unstable.
constexpr std::string_view GOLD_2_3 =
    "04000000000000000e000000000000006275696c646572206661696c6564000002000000000000000100000000000000e8030000000000001204000000000000";
constexpr std::string_view GOLD_2_6 =
    "04000000000000000e000000000000006275696c646572206661696c6564000002000000000000000100000000000000e803000000000000120400000000000001000000000000004b000000000000007368613235363a30303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030216f7574000000000080000000000000007b226964223a227368613235363a30303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030216f7574222c226f757450617468223a222f6e69782f73746f72652f616161616161616161616161616161612d666f6f227d";
constexpr std::string_view GOLD_2_8 =
    "04000000000000000e000000000000006275696c646572206661696c6564000002000000000000000100000000000000e8030000000000001204000000000000010000000000000003000000000000006f757400000000001f000000000000002f6e69782f73746f72652f616161616161616161616161616161612d666f6f00";
constexpr std::string_view GOLD_2_9 =
    "04000000000000000e000000000000006275696c646572206661696c6564000002000000000000000100000000000000e8030000000000001204000000000000010000000000000003000000000000006f757400000000001f000000000000002f6e69782f73746f72652f616161616161616161616161616161612d666f6f0023000000000000002f6e69782f73746f72652f626262626262626262626262626262622d666f6f2e647276000000000005000000000000006275696c64000000010000000000000016000000000000006572726f723a20636f6d6d616e64206661696c65640a0000";

// round-trip at one version: write then read at the same negotiated version.
bool roundtrips(Version v)
{
    auto r = sample();
    std::string bytes = ser(v, r);
    Source src{bytes};
    BuildResult got = read(src, v);
    if (!src.eof())
        return false; // consumed exactly
    bool eq = got.status == r.status && got.errorMsg == r.errorMsg;
    if (v >= V2_3)
        eq = eq && got.timesBuilt == r.timesBuilt && got.isNonDeterministic == r.isNonDeterministic
             && got.startTime == r.startTime && got.stopTime == r.stopTime;
    if (v >= V2_6)
        eq = eq && got.builtOutputs == r.builtOutputs;
    if (v >= V2_9)
        eq = eq && got.logRef == r.logRef && got.failurePhase == r.failurePhase && got.exitCode == r.exitCode
             && got.logTail == r.logTail;
    return eq;
}

} // namespace

// ---- the tests -------------------------------------------------------------

// 1. Round-trip at every version on the ladder.
TEST(ServeDiagCore, roundTripBuildResult)
{
    EXPECT_TRUE(roundtrips(V2_3)) << "round-trip BuildResult @ 2.3";
    EXPECT_TRUE(roundtrips(V2_6)) << "round-trip BuildResult @ 2.6";
    EXPECT_TRUE(roundtrips(V2_8)) << "round-trip BuildResult @ 2.8";
    EXPECT_TRUE(roundtrips(V2_9)) << "round-trip BuildResult @ 2.9";
}

// 2. Golden bytes (characterisation: catch any accidental layout drift).
TEST(ServeDiagCore, goldenBytesStable)
{
    auto r = sample();
    EXPECT_EQ(toHex(ser(V2_3, r)), GOLD_2_3);
    EXPECT_EQ(toHex(ser(V2_6, r)), GOLD_2_6);
    EXPECT_EQ(toHex(ser(V2_8, r)), GOLD_2_8);
    EXPECT_EQ(toHex(ser(V2_9, r)), GOLD_2_9);
}

// 3. The 2.9 layout is purely ADDITIVE: the 2.8 bytes are a prefix of the 2.9
//    bytes (no existing field changes meaning).
TEST(ServeDiagCore, additiveLayout)
{
    auto r = sample();
    std::string b28 = ser(V2_8, r), b29 = ser(V2_9, r);
    EXPECT_GT(b29.size(), b28.size());
    EXPECT_EQ(b29.compare(0, b28.size(), b28), 0) << "2.9 bytes == 2.8 bytes + appended diagnostic-core tail";
}

// 4. 2.8-reads-2.9-bytes: a 2.8 reader consumes EXACTLY the 2.8 fields and stops
//    before the 2.9 tail; the leftover bytes are exactly the diagnostic-core tail.
TEST(ServeDiagCore, readsExactly_2_8_from_2_9_bytes)
{
    auto r = sample();
    std::string b29 = ser(V2_9, r);
    Source src{b29};
    BuildResult got28 = read(src, V2_8); // read with the 2.8 reader
    size_t consumed = src.pos;
    std::string tail = b29.substr(consumed);

    EXPECT_EQ(got28.builtOutputs, r.builtOutputs);
    EXPECT_TRUE(got28.logRef.empty());
    EXPECT_TRUE(got28.logTail.empty());
    EXPECT_EQ(consumed, ser(V2_8, r).size()) << "2.8 reader stops at the 2.8 boundary";

    // the leftover tail is exactly the standalone-serialized 2.9 core
    Sink coreOnly;
    coreOnly.putString(r.logRef);
    coreOnly.putString(r.failurePhase);
    coreOnly.putInt(uint64_t(r.exitCode));
    coreOnly.putString(r.logTail);
    EXPECT_EQ(tail, coreOnly.buf) << "leftover bytes are exactly the diagnostic-core tail";
}

// 5. Negotiated-down peer never EMITS the tail. With min() handshake, a
//    2.9-capable Nix talking to a 2.8 Hydra serializes at 2.8 → no tail.
TEST(ServeDiagCore, negotiatedDownEmitsNoTail)
{
    auto r = sample();
    Version neg = negotiate(V2_9, V2_8);
    EXPECT_TRUE(neg == V2_8) << "min() handshake: 2.9 vs 2.8 -> 2.8";
    EXPECT_EQ(ser(neg, r), ser(V2_8, r)) << "negotiated-down (2.8) emits no diagnostic-core tail";
    EXPECT_FALSE(supportsQueryBuildLog(neg)) << "QueryBuildLog not offered at negotiated 2.8";
}

// 6. Back-compat matrix, both directions — at the COMPATIBLE
//    wire version V2_9 (minor bump within major 2).
TEST(ServeDiagCore, backCompatMatrixBothDirections)
{
    EXPECT_TRUE(negotiate(V2_8, V2_9) == V2_8) << "old Hydra (2.8) <-> new Nix (2.9): negotiate 2.8";
    EXPECT_TRUE(negotiate(V2_9, V2_8) == V2_8) << "new Hydra (2.9) <-> old Nix (2.8): negotiate 2.8";
    EXPECT_TRUE(negotiate(V2_9, V2_9) == V2_9 && supportsQueryBuildLog(negotiate(V2_9, V2_9)))
        << "new <-> new: negotiate 2.9, full diagnostic core active";
}

// 6b. The client handshake guard runs BEFORE min(), so a MAJOR bump is not
//     protected by additivity: a {3,0} server would break every deployed
//     client. That is why the diagnostic core ships as {2,9}, not {3,0}.
TEST(ServeDiagCore, majorBumpRejectedByClientGuardBeforeMin)
{
    EXPECT_FALSE(clientAcceptsServer(V3_0))
        << "old client (major==2) REJECTS a {3,0} builder before min()";
    EXPECT_TRUE(clientAcceptsServer(V2_9)) << "old client ACCEPTS a {2,9} builder (same major)";
    EXPECT_TRUE(negotiate(V2_8, V2_9) == V2_8 && !supportsQueryBuildLog(negotiate(V2_8, V2_9)))
        << "accepted {2,9} builder negotiates {2,8} for the old client — no new fields/op";
    EXPECT_FALSE(clientAcceptsServer(Version{2, 4})) << "client still rejects a below-floor {2,4} server";
}

// 7. QueryBuildLog round-trip + "nix log over serve".
TEST(ServeDiagCore, queryBuildLogRoundTrip)
{
    auto r = sample();
    LogStore store;
    store.logs[r.logRef] = "configuring...\nbuilding...\nerror: command failed\n";

    Sink req;
    writeQueryBuildLogRequest(req, r.logRef);
    Source reqSrc{req.buf};
    EXPECT_EQ(serveQueryBuildLog(reqSrc, store), store.logs[r.logRef])
        << "QueryBuildLog returns the real persisted log (nix log works)";

    Sink req2;
    writeQueryBuildLogRequest(req2, "/nix/store/zzzz-missing.drv");
    Source reqSrc2{req2.buf};
    EXPECT_TRUE(serveQueryBuildLog(reqSrc2, store).empty())
        << "QueryBuildLog for an unknown drv is empty, not an error";
}

// 8. Deferred set stays unstable: the unstable serialization is the frozen 2.9
//    prefix plus builderId/deduplicated — the frozen layout is undisturbed.
TEST(ServeDiagCore, deferredSetAppendsAfterFrozenCore)
{
    auto r = sample();
    std::string b29 = ser(V2_9, r), bU = ser(Vunstable, r);
    EXPECT_GT(bU.size(), b29.size());
    EXPECT_EQ(bU.compare(0, b29.size(), b29), 0)
        << "unstable builderId/deduplicated append AFTER the frozen 2.9 core";
}

} // namespace serve_diag_core_test
