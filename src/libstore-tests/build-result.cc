#include <gtest/gtest.h>

#include "nix/store/build-result.hh"
#include "nix/util/tests/characterization.hh"
#include "nix/util/tests/json-characterization.hh"

namespace nix {

class BuildResultTest : public virtual CharacterizationTest
{
    std::filesystem::path unitTestData = getUnitTestData() / "build-result";

public:
    std::filesystem::path goldenMaster(std::string_view testStem) const override
    {
        return unitTestData / testStem;
    }
};

using nlohmann::json;

struct BuildResultJsonTest : BuildResultTest,
                             JsonCharacterizationTest<BuildResult>,
                             ::testing::WithParamInterface<std::pair<std::string_view, BuildResult>>
{};

TEST_P(BuildResultJsonTest, from_json)
{
    auto & [name, expected] = GetParam();
    readJsonTest(name, expected);
}

TEST_P(BuildResultJsonTest, to_json)
{
    auto & [name, value] = GetParam();
    writeJsonTest(name, value);
}

using namespace std::literals::chrono_literals;

INSTANTIATE_TEST_SUITE_P(
    BuildResultJSON,
    BuildResultJsonTest,
    ::testing::Values(
        std::pair{
            "not-deterministic",
            BuildResult{
                .inner{BuildResult::Failure{{
                    .status = BuildResult::Failure::NotDeterministic,
                    .msg = HintFmt("no idea why"),
                    .isNonDeterministic = false, // Note: This field is separate from the status
                }}},
                .timesBuilt = 1,
            },
        },
        std::pair{
            "output-rejected",
            BuildResult{
                .inner{BuildResult::Failure{{
                    .status = BuildResult::Failure::OutputRejected,
                    .msg = HintFmt("no idea why"),
                    .isNonDeterministic = false,
                }}},
                .timesBuilt = 3,
                .startTime = 30,
                .stopTime = 50,
            },
        },
        std::pair{
            "success",
            BuildResult{
                .inner{BuildResult::Success{
                    .status = BuildResult::Success::Built,
                    .builtOutputs{
                        {
                            "foo",
                            {
                                .outPath = StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo"},
                            },
                        },
                        {
                            "bar",
                            {
                                .outPath = StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar"},
                            },
                        },
                    },
                }},
                .timesBuilt = 3,
                .startTime = 30,
                .stopTime = 50,
                .cpuUser = std::chrono::microseconds(500s),
                .cpuSystem = std::chrono::microseconds(604s),
            },
        },
        std::pair{
            // Deferred dedup/fleet set.
            "deduplicated",
            BuildResult{
                .inner{BuildResult::Success{
                    .status = BuildResult::Success::Built,
                }},
                .timesBuilt = 1,
                .startTime = 30,
                .stopTime = 50,
                .deduplicated = true,
                .builderId = "builder-7",
            },
        },
        std::pair{
            // Failure classification + resource hint (a builder-internal OOM).
            "resource-exhausted",
            BuildResult{
                .inner{BuildResult::Failure{{
                    .status = BuildResult::Failure::MiscFailure,
                    .msg = HintFmt("builder ran out of memory"),
                }}},
                .timesBuilt = 1,
                .exitCode = 137,
                .failureClass = BuildResult::FailureClass::ResourceExhausted,
                .killedForMemory = true,
                .peakMemoryBytes = 4509715456,
            },
        }));

} // namespace nix
