// Workstream D prototype — serve 3.0 diagnostic core (deliverable D1).
//
// THROW-AWAY CODE. Independent of the coordinator prototype (A/B/C): this models
// the *serve protocol* between Hydra and Nix, per Blocker 3 (decisions §"Blocker
// 3", RFC §7, spike §5.1). It is a faithful, standalone model of the existing
// version-gated `BuildResult` serializer ladder in `src/libstore/serve-protocol.cc`
// (the `>= {2,3}/{2,6}/{2,8}` pattern), extended with:
//
//   * the frozen-at-3.0 **diagnostic core** appended AFTER the 2.8 builtOutputs
//     block under a `>= {3,0}` guard, in binary length-prefixed form (NOT JSON):
//     logRef, failurePhase, exitCode, logTail (decisions B3 §3);
//   * `QueryBuildLog` as a new `Command = 10` (next after AddToStoreNar = 9),
//     never sent unless the negotiated version supports it;
//   * the deferred set (builderId, deduplicated) behind an *unstable* gate whose
//     byte layout is explicitly NOT a back-compat promise.
//
// The wire primitives mirror Nix's `serialise.hh`: integers are 8-byte
// little-endian; strings are length (8 bytes) + bytes + zero padding to a
// multiple of 8. So the golden bytes here are directly comparable in shape to
// the real serializer's output.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace serve {

// ---- protocol version (major<<8 | minor), with the min() handshake ----------

struct Version {
    uint8_t major = 0, minor = 0;
    bool operator>=(const Version & o) const {
        return major != o.major ? major > o.major : minor >= o.minor;
    }
    bool operator==(const Version & o) const { return major == o.major && minor == o.minor; }
    uint16_t raw() const { return uint16_t(major) << 8 | minor; }
};

// The handshake takes the minimum of the two peers' versions (decisions B3 §3
// back-compat matrix): both sides then speak exactly that version.
inline Version negotiate(Version a, Version b) { return (a >= b) ? b : a; }

// Known versions on the ladder.
inline constexpr Version V2_3{2, 3};
inline constexpr Version V2_6{2, 6};
inline constexpr Version V2_8{2, 8};   // current SERVE_PROTOCOL_VERSION
inline constexpr Version V3_0{3, 0};   // serve 3.0 — frozen diagnostic core
inline constexpr Version V3_unstable{3, 99}; // deferred builderId/deduplicated (NOT frozen)

// ---- serve Command enum (the new op is appended, never renumbered) ----------

enum class Command : uint64_t {
    QueryValidPaths   = 1,
    QueryPathInfos    = 2,
    DumpStorePath     = 3,
    ImportPaths       = 4,
    ExportPaths       = 5,
    BuildPaths        = 6,
    QueryClosure      = 7,
    BuildDerivation   = 8,
    AddToStoreNar     = 9,
    QueryBuildLog     = 10,  // NEW (decisions B3 §3); guarded by >= {3,0}
};

inline bool supportsQueryBuildLog(Version negotiated) { return negotiated >= V3_0; }

// ---- wire primitives (Nix serialise.hh shape) ------------------------------

struct WireError : std::runtime_error { using std::runtime_error::runtime_error; };

struct Sink {
    std::string buf;
    void putInt(uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back(char((v >> (i * 8)) & 0xff)); } // LE
    void putString(std::string_view s) {
        putInt(s.size());
        buf.append(s);
        while (buf.size() % 8 != 0) buf.push_back('\0');   // pad to 8
    }
};

struct Source {
    std::string_view s;
    size_t pos = 0;
    bool eof() const { return pos >= s.size(); }
    uint64_t getInt() {
        if (pos + 8 > s.size()) throw WireError("short read (int)");
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(s[pos + i])) << (i * 8);
        pos += 8;
        return v;
    }
    std::string getString() {
        uint64_t n = getInt();
        if (pos + n > s.size()) throw WireError("short read (string)");
        std::string out(s.substr(pos, n));
        pos += n;
        while (pos % 8 != 0) ++pos;   // skip padding
        return out;
    }
};

// ---- the BuildResult model -------------------------------------------------

enum class Status : uint64_t { Built = 0, Substituted = 1, PermanentFailure = 4, OutputRejected = 6 };

struct BuildResult {
    Status status = Status::Built;
    std::string errorMsg;                 // failure message (empty on success)

    // >= {2,3}
    uint64_t timesBuilt = 0;
    uint8_t  isNonDeterministic = 0;
    uint64_t startTime = 0;
    uint64_t stopTime = 0;

    // >= {2,8} binary (>= {2,6} JSON-hack): outputName -> outPath
    std::map<std::string, std::string> builtOutputs;

    // >= {3,0} — the FROZEN diagnostic core (decisions B3 §3)
    std::string logRef;        // resolved drv path the builder persisted the log under
    std::string failurePhase;  // "" if not a failure / unknown
    int64_t     exitCode = 0;
    std::string logTail;

    // unstable (>= {3,99}) — DEFERRED, layout NOT promised (builderId, deduplicated)
    std::string builderId;
    uint8_t     deduplicated = 0;
};

inline std::string dummyHash() { return std::string("sha256:") + std::string(64, '0'); }

// write — mirrors serve-protocol.cc Serialise<BuildResult>::write
inline void write(Sink & to, Version v, const BuildResult & res)
{
    to.putInt(uint64_t(res.status));
    to.putString(res.errorMsg);

    if (v >= V2_3) {
        to.putInt(res.timesBuilt);
        to.putInt(res.isNonDeterministic);
        to.putInt(res.startTime);
        to.putInt(res.stopTime);
    }

    if (v >= V2_8) {                                   // binary map
        to.putInt(res.builtOutputs.size());
        for (auto & [name, outPath] : res.builtOutputs) { to.putString(name); to.putString(outPath); }
    } else if (v >= V2_6) {                            // legacy JSON-hack StringMap
        to.putInt(res.builtOutputs.size());
        for (auto & [name, outPath] : res.builtOutputs) {
            std::string id = dummyHash() + "!" + name;
            std::string j = "{\"id\":\"" + id + "\",\"outPath\":\"" + outPath + "\"}";
            to.putString(id);
            to.putString(j);
        }
    }

    // 3.0 diagnostic core, appended AFTER builtOutputs, binary, >= {3,0}.
    if (v >= V3_0) {
        to.putString(res.logRef);
        to.putString(res.failurePhase);
        to.putInt(uint64_t(res.exitCode));
        to.putString(res.logTail);
    }

    // Deferred/unstable set — only on the explicitly-unstable version.
    if (v >= V3_unstable) {
        to.putString(res.builderId);
        to.putInt(res.deduplicated);
    }
}

// read — mirrors serve-protocol.cc Serialise<BuildResult>::read
inline BuildResult read(Source & from, Version v)
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
        for (uint64_t i = 0; i < n; ++i) { auto name = from.getString(); auto outPath = from.getString(); res.builtOutputs[name] = outPath; }
    } else if (v >= V2_6) {
        uint64_t n = from.getInt();
        for (uint64_t i = 0; i < n; ++i) {
            std::string id = from.getString();
            std::string j = from.getString();
            // old reader extracts only outputName (after '!') and outPath
            size_t bang = id.find('!');
            std::string name = bang == std::string::npos ? id : id.substr(bang + 1);
            size_t k = j.find("\"outPath\":\"");
            std::string outPath;
            if (k != std::string::npos) { k += 11; outPath = j.substr(k, j.find('"', k) - k); }
            res.builtOutputs[name] = outPath;
        }
    }

    if (v >= V3_0) {
        res.logRef = from.getString();
        res.failurePhase = from.getString();
        res.exitCode = int64_t(from.getInt());
        res.logTail = from.getString();
    }

    if (v >= V3_unstable) {
        res.builderId = from.getString();
        res.deduplicated = uint8_t(from.getInt());
    }

    return res;
}

inline std::string toHex(std::string_view b)
{
    static const char * h = "0123456789abcdef";
    std::string out;
    for (unsigned char c : b) { out.push_back(h[c >> 4]); out.push_back(h[c & 0xf]); }
    return out;
}

// ---- QueryBuildLog (Command = 10) ------------------------------------------
//
// Models the Gap A / §4.5 fix: `getBuildLogExact` over the serve path. A client
// at a negotiated version >= {3,0} may send QueryBuildLog{drvPath}; the server
// replies with the persisted log bytes (or empty if none). A negotiated-down
// (<= 2.8) client must NOT send it and falls back to out-of-band capture.

struct LogStore {                       // stand-in for the builder's persisted logs
    std::map<std::string, std::string> logs;   // logRef -> log contents
    std::string get(const std::string & logRef) const {
        auto it = logs.find(logRef);
        return it == logs.end() ? std::string() : it->second;
    }
};

// Encode/decode a QueryBuildLog request + response over the wire.
inline void writeQueryBuildLogRequest(Sink & to, const std::string & drvPath)
{
    to.putInt(uint64_t(Command::QueryBuildLog));
    to.putString(drvPath);
}

inline std::string serveQueryBuildLog(Source & from, const LogStore & store)
{
    auto cmd = Command(from.getInt());
    if (cmd != Command::QueryBuildLog) throw WireError("unexpected command");
    std::string drvPath = from.getString();
    return store.get(drvPath);
}

} // namespace serve
