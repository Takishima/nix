#include "nix/store/serve-protocol-connection.hh"
#include "nix/store/serve-protocol-impl.hh"
#include "nix/store/build-result.hh"
#include "nix/store/derivations.hh"
#include "nix/store/worker-protocol.hh" // for the STDERR_* log-tunnel framing
#include "nix/util/experimental-features.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh" // chomp

namespace nix {

/* Read serve log-tunnel fields; mirrors `writeServeLogFields` on the server
   (nix-store.cc) and the worker protocol's `readFields`. */
static Logger::Fields readServeLogFields(Source & from)
{
    Logger::Fields fields;
    size_t size = readInt(from);
    for (size_t n = 0; n < size; n++) {
        auto type = (decltype(Logger::Field::type)) readInt(from);
        if (type == Logger::Field::tInt)
            fields.push_back(readNum<uint64_t>(from));
        else if (type == Logger::Field::tString)
            fields.push_back(readString(from));
        else
            throw Error("got unsupported field type %x in serve log stream", (int) type);
    }
    return fields;
}

void ServeProto::BasicClientConnection::processStderr()
{
    /* Replay the server's STDERR_* log-frame stream into the ambient logger
       until STDERR_LAST, after which the build result follows. The result/error
       encoding is unchanged; this only drains
       the log frames the server now prepends on the unstable serve surface. */
    while (true) {
        auto msg = readNum<uint64_t>(from);
        if (msg == STDERR_NEXT) {
            printError(chomp(readString(from)));
        } else if (msg == STDERR_START_ACTIVITY) {
            auto act = readNum<ActivityId>(from);
            auto lvl = (Verbosity) readInt(from);
            auto type = (ActivityType) readInt(from);
            auto s = readString(from);
            auto fields = readServeLogFields(from);
            auto parent = readNum<ActivityId>(from);
            logger->startActivity(act, lvl, type, s, fields, parent);
        } else if (msg == STDERR_STOP_ACTIVITY) {
            auto act = readNum<ActivityId>(from);
            logger->stopActivity(act);
        } else if (msg == STDERR_RESULT) {
            auto act = readNum<ActivityId>(from);
            auto type = (ResultType) readInt(from);
            auto fields = readServeLogFields(from);
            logger->result(act, type, fields);
        } else if (msg == STDERR_LAST) {
            break;
        } else {
            throw Error("got unknown message type %x from remote serve builder", msg);
        }
    }
}

ServeProto::Version ServeProto::offeredVersion()
{
    // Only offer the provisional 2.9 diagnostic surface when explicitly opted
    // in; otherwise stay at the stable `latest` (2.8). The `min()` handshake
    // then degrades transparently for peers that do not offer 2.9.
    return experimentalFeatureSettings.isEnabled(Xp::ServeBuildLogs) ? ServeProto::unstableDiagnostics
                                                                     : ServeProto::latest;
}

ServeProto::Version ServeProto::BasicClientConnection::handshake(
    BufferedSink & to, Source & from, ServeProto::Version localVersion, std::string_view host)
{
    to << SERVE_MAGIC_1 << localVersion.toWire();
    to.flush();

    unsigned int magic = readInt(from);
    if (magic != SERVE_MAGIC_2)
        throw Error("'nix-store --serve' protocol mismatch from '%s'", host);
    auto remoteVersion = ServeProto::Version::fromWire(readInt(from));
    if (remoteVersion.major != 2 || remoteVersion < ServeProto::Version{2, 5})
        throw Error("unsupported 'nix-store --serve' protocol version on '%s'", host);
    return std::min(remoteVersion, localVersion);
}

ServeProto::Version
ServeProto::BasicServerConnection::handshake(BufferedSink & to, Source & from, ServeProto::Version localVersion)
{
    unsigned int magic = readInt(from);
    if (magic != SERVE_MAGIC_1)
        throw Error("protocol mismatch");
    to << SERVE_MAGIC_2 << localVersion.toWire();
    to.flush();
    auto remoteVersion = ServeProto::Version::fromWire(readInt(from));
    return std::min(remoteVersion, localVersion);
}

StorePathSet ServeProto::BasicClientConnection::queryValidPaths(
    const StoreDirConfig & store, bool lock, const StorePathSet & paths, SubstituteFlag maybeSubstitute)
{
    to << ServeProto::Command::QueryValidPaths << lock << maybeSubstitute;
    write(store, *this, paths);
    to.flush();

    return Serialise<StorePathSet>::read(store, *this);
}

std::map<StorePath, UnkeyedValidPathInfo>
ServeProto::BasicClientConnection::queryPathInfos(const StoreDirConfig & store, const StorePathSet & paths)
{
    std::map<StorePath, UnkeyedValidPathInfo> infos;

    to << ServeProto::Command::QueryPathInfos;
    ServeProto::write(store, *this, paths);
    to.flush();

    while (true) {
        auto storePathS = readString(from);
        if (storePathS == "")
            break;

        auto storePath = store.parseStorePath(storePathS);
        assert(paths.count(storePath) == 1);
        auto info = ServeProto::Serialise<UnkeyedValidPathInfo>::read(store, *this);
        infos.insert_or_assign(std::move(storePath), std::move(info));
    }

    return infos;
}

void ServeProto::BasicClientConnection::putBuildDerivationRequest(
    const StoreDirConfig & store,
    const StorePath & drvPath,
    const BasicDerivation & drv,
    const ServeProto::BuildOptions & options)
{
    to << ServeProto::Command::BuildDerivation << store.printStorePath(drvPath);
    writeDerivation(to, store, drv);

    ServeProto::write(store, *this, options);

    to.flush();
}

BuildResult ServeProto::BasicClientConnection::getBuildDerivationResponse(const StoreDirConfig & store)
{
    // Drain the live log stream (serve >= 2.9) before the result.
    if (ServeProto::supportsDiagnostics(remoteVersion))
        processStderr();
    return ServeProto::Serialise<BuildResult>::read(store, *this);
}

void ServeProto::BasicClientConnection::narFromPath(
    const StoreDirConfig & store, const StorePath & path, fun<void(Source &)> receiveNar)
{
    to << ServeProto::Command::DumpStorePath << store.printStorePath(path);
    to.flush();

    receiveNar(from);
}

void ServeProto::BasicClientConnection::importPaths(const StoreDirConfig & store, fun<void(Sink &)> sendPaths)
{
    to << ServeProto::Command::ImportPaths;
    sendPaths(to);
    to.flush();

    if (readInt(from) != 1)
        throw Error("remote machine failed to import closure");
}

} // namespace nix
