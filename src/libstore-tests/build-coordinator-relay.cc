#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "nix/store/build-result.hh"
#include "nix/store/build/build-coordinator.hh"
#include "nix/store/serve-protocol-log-tunnel.hh"
#include "nix/store/worker-protocol.hh" // STDERR_* framing constants
#include "nix/util/finally.hh"
#include "nix/util/serialise.hh"

namespace nix {

namespace {

/* Coordinator records as the coordinator frames them (tag byte + payload;
   `readRecord` strips the length prefix before the records get here). */

std::string frameRec(std::string_view data)
{
    std::string s;
    s.push_back(coordinator_proto::MSG_FRAME);
    s.push_back(0); // not replayed
    s += data;
    return s;
}

std::string resultRec(const BuildResult & res)
{
    std::string s;
    s.push_back(coordinator_proto::MSG_RESULT);
    s += nlohmann::json(res).dump();
    return s;
}

BuildResult successResult()
{
    BuildResult res;
    res.inner = BuildResult::Success{.status = BuildResult::Success::Built};
    return res;
}

/** Feed a fixed record sequence through `processCoordinatorRelayRecords`. */
BuildResult pump(const std::vector<std::string> & recs, Logger & logger)
{
    size_t i = 0;
    return processCoordinatorRelayRecords(
        [&]() -> std::optional<std::string> {
            if (i < recs.size())
                return recs[i++];
            return std::nullopt;
        },
        logger,
        "/nix/store/g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x.drv");
}

/** An in-memory BufferedSink, standing in for the serve connection's fd. */
struct BufStringSink : BufferedSink
{
    std::string s;

    void writeUnbuffered(std::string_view data) override
    {
        s.append(data);
    }
};

/* Client-side decoding of the serve STDERR_* stream (mirrors
   `ServeProto::BasicClientConnection::processStderr`). */

struct DecodedWire
{
    std::vector<std::string> rawNextLines;     // STDERR_NEXT payloads
    std::vector<std::string> buildLogLines;    // resBuildLogLine result fields
    size_t activitiesStarted = 0;
};

Logger::Fields readFields(Source & from)
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
            throw Error("unsupported field type in test wire");
    }
    return fields;
}

DecodedWire decodeServeWire(std::string_view wire)
{
    DecodedWire out;
    StringSource from{wire};
    while (true) {
        auto msg = readNum<uint64_t>(from);
        if (msg == STDERR_LAST)
            break;
        else if (msg == STDERR_NEXT)
            out.rawNextLines.push_back(readString(from));
        else if (msg == STDERR_START_ACTIVITY) {
            readNum<ActivityId>(from); // act
            readInt(from);             // lvl
            readInt(from);             // type
            readString(from);          // s
            readFields(from);
            readNum<ActivityId>(from); // parent
            out.activitiesStarted++;
        } else if (msg == STDERR_STOP_ACTIVITY)
            readNum<ActivityId>(from);
        else if (msg == STDERR_RESULT) {
            readNum<ActivityId>(from);
            auto type = (ResultType) readInt(from);
            auto fields = readFields(from);
            if (type == resBuildLogLine && !fields.empty() && fields[0].type == Logger::Field::tString)
                out.buildLogLines.push_back(fields[0].s);
        } else
            throw Error("unknown message type %x in test wire", msg);
    }
    return out;
}

} // namespace

/* Regression: a coordinator-relayed build served over `nix-store --serve`
   (which pins `verbosity = lvlError`) delivered zero log lines, because the
   relay used `log(lvlInfo)` and the serve tunnel drops that at `lvlError`. */
TEST(CoordinatorRelay, framesSurviveServeTunnelAtLvlError)
{
    auto savedVerbosity = verbosity;
    verbosity = lvlError; // what `nix-store --serve` getBuildSettings() pins
    Finally restore([&]() { verbosity = savedVerbosity; });

    BufStringSink wire;
    ServeTunnelLogger tunnel(wire);
    tunnel.startWork();

    auto res = pump({frameRec("hello-from-the-builder\n"), frameRec("second-line\n"), resultRec(successResult())}, tunnel);

    tunnel.stopWork();
    wire.flush();

    EXPECT_TRUE(res.tryGetSuccess() != nullptr);

    auto decoded = decodeServeWire(wire.s);
    // The whole point: the client must receive the build log lines.
    ASSERT_EQ(decoded.buildLogLines.size(), 2u);
    EXPECT_EQ(decoded.buildLogLines[0], "hello-from-the-builder");
    EXPECT_EQ(decoded.buildLogLines[1], "second-line");
    // ... under a started activity, so progress-bar clients can attribute them.
    EXPECT_GE(decoded.activitiesStarted, 1u);
}

/* Relayed lines must be `resBuildLogLine` results so each client renders them
   per its own `-L`, like a non-relayed build. */
TEST(CoordinatorRelay, framesAreBuildLogLineResults)
{
    struct CapturingLogger : Logger
    {
        std::vector<std::string> logLines;
        std::vector<std::string> buildLogLines;

        void log(Verbosity, std::string_view s) override
        {
            logLines.emplace_back(s);
        }

        void logEI(const ErrorInfo &) override {}

        void result(ActivityId, ResultType type, const Fields & fields) override
        {
            if (type == resBuildLogLine && !fields.empty() && fields[0].type == Logger::Field::tString)
                buildLogLines.push_back(fields[0].s);
        }
    };

    CapturingLogger capture;
    auto res = pump({frameRec("a line\n"), resultRec(successResult())}, capture);

    EXPECT_TRUE(res.tryGetSuccess() != nullptr);
    ASSERT_EQ(capture.buildLogLines.size(), 1u);
    EXPECT_EQ(capture.buildLogLines[0], "a line");
}

/* The event-loop relay (`CoordinatorRelayPump::feed`) must decode the
   length-prefixed byte stream incrementally: records arrive in arbitrary
   splits (a socket read can end mid-length-prefix, mid-record, or carry
   several records), and the decoded frames/result must be exactly those of
   the record-at-a-time path. */
TEST(CoordinatorRelay, incrementalByteFeedDecodesAcrossSplits)
{
    // The raw byte stream as the coordinator writes it.
    auto frameBytes = [](std::string_view body) {
        uint32_t len = (uint32_t) body.size();
        std::string s;
        s.resize(4);
        memcpy(s.data(), &len, 4);
        s += body;
        return s;
    };
    std::string wireBytes = frameBytes(frameRec("one\ntwo\n")) + frameBytes(frameRec("three"))
                            + frameBytes(frameRec("-and-more\n")) + frameBytes(resultRec(successResult()));

    // Feed it in every chunk size from pathological (1 byte) upwards.
    for (size_t chunk : {(size_t) 1, (size_t) 3, (size_t) 7, wireBytes.size()}) {
        BufStringSink wire;
        ServeTunnelLogger tunnel(wire);
        tunnel.startWork();

        std::optional<BuildResult> res;
        {
            CoordinatorRelayPump pump(tunnel, "/nix/store/g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-x.drv");
            for (size_t i = 0; i < wireBytes.size() && !res; i += chunk)
                res = pump.feed(std::string_view(wireBytes).substr(i, chunk));
        }

        tunnel.stopWork();
        wire.flush();

        ASSERT_TRUE(res.has_value()) << "chunk size " << chunk;
        EXPECT_TRUE(res->tryGetSuccess() != nullptr) << "chunk size " << chunk;

        auto decoded = decodeServeWire(wire.s);
        ASSERT_EQ(decoded.buildLogLines.size(), 3u) << "chunk size " << chunk;
        EXPECT_EQ(decoded.buildLogLines[0], "one");
        EXPECT_EQ(decoded.buildLogLines[1], "two");
        EXPECT_EQ(decoded.buildLogLines[2], "three-and-more");
    }
}

/* `resBuildLogLine` is line-oriented; multi-line frames must be split. */
TEST(CoordinatorRelay, multiLineFramesAreSplit)
{
    BufStringSink wire;
    ServeTunnelLogger tunnel(wire);
    tunnel.startWork();

    pump({frameRec("one\ntwo\n"), resultRec(successResult())}, tunnel);

    tunnel.stopWork();
    wire.flush();

    auto decoded = decodeServeWire(wire.s);
    ASSERT_EQ(decoded.buildLogLines.size(), 2u);
    EXPECT_EQ(decoded.buildLogLines[0], "one");
    EXPECT_EQ(decoded.buildLogLines[1], "two");
}

} // namespace nix
