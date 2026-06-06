// Workstream A prototype — shared control protocol + socket helpers.
//
// THROW-AWAY CODE. This is the experimental branch the validation plan
// (doc/rfcs/remote-build-protocol-redesign.validation.md, Workstream A) and the
// spike (spike.md §3, §4) call for. It depends on nothing in the Nix tree on
// purpose: the spike's whole point is that the coordinator lives *below* the
// public wire, so it can be modelled in isolation. None of this is meant to be
// merged into libstore/daemon as-is.
//
// The control protocol here is the child<->coordinator protocol of spike §3.2.
// It is deliberately *not* the worker/serve protocol; it never reaches a client.
// Every message is length-prefixed ([u32 len][body]) so the single-threaded,
// fully non-blocking coordinator event loop can frame messages without ever
// blocking on a partial read.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <stdexcept>
#include <optional>

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>

namespace wsa {

// ---- control protocol (child -> coordinator), spike §3.2 -------------------

enum class Op : uint8_t {
    StartOrAttach = 1, // { buildKey, drvForBuild, sessionAuth, replayWanted, builderSpec }
    Subscribe     = 2, // { subscriptionId } -- begins the frame stream
    Unsubscribe   = 3, // { subscriptionId } -- detach, decrement refcount
    CancelHint    = 4, // { subscriptionId } -- "my client actively cancelled"
    QueryActive   = 5, // { sessionAuth }    -> ActiveList
};

// ---- coordinator -> child stream messages ---------------------------------

enum class Msg : uint8_t {
    StartReply   = 64, // { status, subscriptionId, deduplicated }  (reply to StartOrAttach)
    Frame        = 65, // { replayed, bytes }  -- relayed verbatim to the client socket
    BuildResult  = 66, // { success, exitCode, deduplicated, logRef }
    AttachState  = 67, // { state }  (resolving | building | finished), spike §3.8
    ActiveList   = 68, // { count, [ {key, subscribers, logBytes} ... ] }  spike §3.9
};

enum class Status  : uint8_t { Ok = 0, Denied = 1 };               // authorize(), spike §3.7
enum class AState  : uint8_t { Resolving = 0, Building = 1, Finished = 2 };

// The build key (spike §3.2/§3.3) plus, for the prototype, the parameters of the
// deliberately-slow builder (A3) so the coordinator can fork it.
struct BuilderSpec {
    std::string buildKey;     // resolved-drv stand-in; registry is keyed on this
    std::string drvForBuild;  // what the caller is authorized to build
    std::string counterFile;  // A3: incremented once per *actual* build (dedup proof)
    uint32_t    nLines = 0;    // how many marker lines the builder emits
    uint32_t    sleepMs = 0;   // per-line sleep so a late joiner reliably attaches
};

// The authenticated identity the child reports (spike §3.7.2). The coordinator
// trusts this only after peer-verifying the child via SO_PEERCRED (§3.7.1), and
// re-derives authorization itself (§3.7).
struct SessionAuth {
    uint32_t uid = 0;
    uint8_t  trusted = 0;
};

// Authorization policy (spike §3.7.2), re-derived by the coordinator from drv
// *material* (never delegated to the child). Toy policy, but rich enough for the
// Workstream B trust tests:
//   * trusted callers may build anything;
//   * a drv carrying an "allow=<uid,uid,...>" token authorizes exactly those uids
//     (this is what lets a CA *resolved* key be authorized for one caller and not
//     another — the T2 re-auth-on-promotion case);
//   * otherwise an untrusted caller may build only CA-prefixed ("ca:") drvs.
inline bool authorizeFor(const SessionAuth & a, const std::string & material)
{
    if (a.trusted) return true;
    auto pos = material.find("allow=");
    if (pos != std::string::npos) {
        std::string list = material.substr(pos + 6);
        if (auto sc = list.find(';'); sc != std::string::npos) list = list.substr(0, sc);
        std::string me = std::to_string(a.uid);
        size_t i = 0;
        while (i < list.size()) {
            size_t c = list.find(',', i);
            std::string tok = list.substr(i, c == std::string::npos ? std::string::npos : c - i);
            if (tok == me) return true;
            if (c == std::string::npos) break;
            i = c + 1;
        }
        return false;
    }
    return material.rfind("ca:", 0) == 0;
}

// ---- length-prefixed framing ----------------------------------------------

// Append-only writer over a std::string body (no length prefix; see frame()).
struct BufWriter {
    std::string buf;
    void u8(uint8_t v)  { buf.push_back(static_cast<char>(v)); }
    void u32(uint32_t v) { for (int i = 3; i >= 0; --i) buf.push_back(char((v >> (i * 8)) & 0xff)); }
    void u64(uint64_t v) { for (int i = 7; i >= 0; --i) buf.push_back(char((v >> (i * 8)) & 0xff)); }
    void str(std::string_view s) { u32(uint32_t(s.size())); buf.append(s); }
};

struct ProtoError : std::runtime_error { using std::runtime_error::runtime_error; };

// Cursor reader over a *complete* message body. Because messages are only parsed
// once fully buffered, there are no partial-read concerns inside here.
struct BufReader {
    std::string_view s;
    size_t pos = 0;
    void need(size_t n) const { if (pos + n > s.size()) throw ProtoError("short message"); }
    uint8_t u8()  { need(1); return uint8_t(s[pos++]); }
    uint32_t u32() { need(4); uint32_t v = 0; for (int i = 0; i < 4; ++i) v = (v << 8) | uint8_t(s[pos++]); return v; }
    uint64_t u64() { need(8); uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | uint8_t(s[pos++]); return v; }
    std::string str() { uint32_t n = u32(); need(n); auto r = std::string(s.substr(pos, n)); pos += n; return r; }
};

// Wrap a body in its length prefix, ready to append to an outbound queue.
inline std::string frame(const std::string & body)
{
    std::string out;
    uint32_t n = uint32_t(body.size());
    for (int i = 3; i >= 0; --i) out.push_back(char((n >> (i * 8)) & 0xff));
    out.append(body);
    return out;
}

// Pull one complete framed message out of an accumulating input buffer.
// Returns the body (without prefix) and erases it from `in`, or nullopt if a
// whole message is not yet available.
inline std::optional<std::string> takeFrame(std::string & in)
{
    if (in.size() < 4) return std::nullopt;
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n = (n << 8) | uint8_t(in[i]);
    if (in.size() < 4 + size_t(n)) return std::nullopt;
    std::string body = in.substr(4, n);
    in.erase(0, 4 + size_t(n));
    return body;
}

// ---- client-facing records (relay child <-> test client) -------------------
//
// Stand-in for the *public* worker-protocol surface (§5.1): the relay maps each
// coordinator FRAME -> CRec::Log (preserving the replayed flag, RFC §4.3.1) and
// BUILD_RESULT -> CRec::Result. This is the only thing a real client would ever
// see; it is identical whether the bytes came from a coordinator-relayed child
// or a local build, which is the spike's no-flag-day property (§5.1).

enum class CRec : uint8_t {
    Log    = 1, // { replayed, bytes }
    Result = 2, // { ok, exitCode, deduplicated, logRef }
    Denied = 3, // authorize() refused (no existence info leaked)
};

// client -> relay child: the build request.
//
// Workstream B (CA key-merge, spike §3.8) adds the CA fields. For an
// input-addressed build (ca==0) the build key is `drvForBuild` and the resolve
// phase is skipped. For a CA build (ca==1) the coordinator registers a
// short-lived `resolving` provisional entry keyed on `unresolvedDrv`, then after
// `resolveMs` promotes/merges onto the *resolved* key (`resolvedDrv`) and
// re-authorizes every subscriber against it. `buildKey` is the client's
// *asserted* key, used only for spoof detection (T3): the coordinator recomputes
// the canonical key from the drv material it received and rejects a mismatch.
struct ClientRequest {
    std::string buildKey;         // asserted key (T3 spoof check); honest = key material
    std::string drvForBuild;      // what the caller is authorized to build
    uint32_t    uid = 0;          // identity the child reports as sessionAuth
    uint8_t     trusted = 0;
    uint8_t     replayWanted = 1;
    uint8_t     explicitRoot = 0; // §5.2 / C-c keep-alive
    std::string counterFile;
    uint32_t    nLines = 0;
    uint32_t    sleepMs = 0;
    uint8_t     behavior = 0;     // 0 normal, 1 slow-reader, 2 disconnect-after-first
    // --- Workstream B / CA (spike §3.8) ---
    uint8_t     ca = 0;           // 1 = content-addressed: go through resolve+promote
    std::string unresolvedDrv;    // provisional registry key during `resolving`
    std::string resolvedDrv;      // resolved drv material -> canonical build key
    uint32_t    resolveMs = 0;    // how long the resolve phase takes (test knob)

    std::string encode() const {
        BufWriter w;
        w.str(buildKey); w.str(drvForBuild); w.u32(uid); w.u8(trusted);
        w.u8(replayWanted); w.u8(explicitRoot); w.str(counterFile);
        w.u32(nLines); w.u32(sleepMs); w.u8(behavior);
        w.u8(ca); w.str(unresolvedDrv); w.str(resolvedDrv); w.u32(resolveMs);
        return w.buf;
    }
    static ClientRequest decode(const std::string & body) {
        BufReader r{body}; ClientRequest q;
        q.buildKey = r.str(); q.drvForBuild = r.str(); q.uid = r.u32(); q.trusted = r.u8();
        q.replayWanted = r.u8(); q.explicitRoot = r.u8(); q.counterFile = r.str();
        q.nLines = r.u32(); q.sleepMs = r.u32(); q.behavior = r.u8();
        q.ca = r.u8(); q.unresolvedDrv = r.str(); q.resolvedDrv = r.str(); q.resolveMs = r.u32();
        return q;
    }
};

// ---- socket helpers --------------------------------------------------------

inline void setNonBlocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        throw std::runtime_error(std::string("fcntl O_NONBLOCK: ") + strerror(errno));
}

// Shrink kernel socket buffers when WSA_SOCKBUF is set, so the §3.6 backpressure
// path (a slow subscriber backing up into the coordinator's per-session queue)
// is reachable with a small, fast test rather than megabytes of log.
inline void setSockBuf(int fd)
{
    const char * v = ::getenv("WSA_SOCKBUF");
    if (!v) return;
    int sz = 0; try { sz = std::stoi(v); } catch (...) { return; }
    if (sz <= 0) return;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
}

inline void setCloexec(int fd)
{
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl < 0 || fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0)
        throw std::runtime_error(std::string("fcntl FD_CLOEXEC: ") + strerror(errno));
}

inline void fillAddr(sockaddr_un & addr, const std::string & path)
{
    if (path.size() >= sizeof(addr.sun_path))
        throw std::runtime_error("socket path too long: " + path);
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size());
}

// Peer credentials from SO_PEERCRED -- the §3.7.1 linchpin. The coordinator
// refuses any peer whose uid is not the daemon's own uid before reading a byte
// of sessionAuth.
struct PeerCred { uid_t uid; gid_t gid; pid_t pid; };

inline PeerCred getPeerCred(int fd)
{
    ucred uc{};
    socklen_t len = sizeof(uc);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) < 0)
        throw std::runtime_error(std::string("SO_PEERCRED: ") + strerror(errno));
    return { uc.uid, uc.gid, uc.pid };
}

// Create a listening Unix socket at `path`, mode 0660 (spike §3.7.1).
inline int makeListenSocket(const std::string & path, int backlog = 64)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error(std::string("socket: ") + strerror(errno));
    setCloexec(fd);
    ::unlink(path.c_str());
    sockaddr_un addr;
    fillAddr(addr, path);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        int e = errno; ::close(fd);
        throw std::runtime_error(std::string("bind ") + path + ": " + strerror(e));
    }
    ::chmod(path.c_str(), 0660);
    if (listen(fd, backlog) < 0) {
        int e = errno; ::close(fd);
        throw std::runtime_error(std::string("listen: ") + strerror(e));
    }
    return fd;
}

// Connect to a Unix socket. Returns -1 (caller inspects errno) on failure so the
// election / decline-and-respawn races (spike §3.1, O3) can be handled.
inline int connectSocket(const std::string & path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setCloexec(fd);
    sockaddr_un addr;
    fillAddr(addr, path);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        int e = errno; ::close(fd); errno = e;
        return -1;
    }
    return fd;
}

// Blocking write of an entire buffer (used by the simple relay/client handshake;
// the coordinator never blocks -- it queues, see coordinator.cc).
inline bool writeAllBlocking(int fd, std::string_view s)
{
    while (!s.empty()) {
        ssize_t n = ::write(fd, s.data(), s.size());
        if (n < 0) { if (errno == EINTR) continue; return false; }
        s.remove_prefix(size_t(n));
    }
    return true;
}

// Blocking read of one whole framed message. Returns body, or nullopt on EOF.
inline std::optional<std::string> readFrameBlocking(int fd)
{
    std::string in;
    char tmp[4096];
    for (;;) {
        if (auto body = takeFrame(in)) return body;
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n == 0) return std::nullopt;          // EOF (peer/coordinator gone)
        if (n < 0) { if (errno == EINTR) continue; return std::nullopt; }
        in.append(tmp, size_t(n));
    }
}

} // namespace wsa
