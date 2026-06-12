#include <gtest/gtest.h>

#include "nix/store/build/build-registry.hh"

namespace nix {

/* ------------------------------------------------------------------------ *
 * Test helpers
 * ------------------------------------------------------------------------ */

namespace {

BuildRegistryKey key(std::string s)
{
    return BuildRegistryKey{.resolvedDrv = std::move(s)};
}

/** A subscriber's captured stream: its log frames and (eventual) result. */
struct Recorder
{
    std::vector<BuildLogFrame> frames;
    std::optional<BuildResult> result;

    BuildLogSink logSink()
    {
        return [this](const BuildLogFrame & f) { frames.push_back(f); };
    }

    BuildResultSink resultSink()
    {
        return [this](const BuildResult & r) { result = r; };
    }

    std::vector<std::string> replayedData() const
    {
        std::vector<std::string> out;
        for (auto & f : frames)
            if (f.replayed)
                out.push_back(f.data);
        return out;
    }

    std::vector<std::string> liveData() const
    {
        std::vector<std::string> out;
        for (auto & f : frames)
            if (!f.replayed)
                out.push_back(f.data);
        return out;
    }
};

BuildResult registrySuccessResult()
{
    BuildResult res;
    res.inner = BuildResult::Success{.status = BuildResult::Success::Built};
    return res;
}

/** A policy under which only trusted identities may build (the existing daemon
 *  notion that untrusted users cannot build arbitrary derivations). Used to
 *  exercise authorize-before-registry and the no-existence-oracle property. */
struct TrustedOnlyPolicy : BuildAuthPolicy
{
    bool mayBuild(const BuildAuth & auth, const BuildRegistryKey &) const override
    {
        return auth.trusted;
    }
};

} // namespace

/* ------------------------------------------------------------------------ *
 * Dedup / coalescing
 * ------------------------------------------------------------------------ */

TEST(BuildRegistry, dedupCoalescesSameKey)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a, b;
    auto ra = reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {});
    auto rb = reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), {});

    ASSERT_TRUE(ra);
    ASSERT_TRUE(rb);
    // Exactly one build started; the second coalesced (a HIT → deduplicated).
    EXPECT_TRUE(ra->started);
    EXPECT_FALSE(ra->deduplicated);
    EXPECT_FALSE(rb->started);
    EXPECT_TRUE(rb->deduplicated);
    EXPECT_TRUE(reg->isLive(key("k")));
}

TEST(BuildRegistry, distinctKeysDoNotCoalesce)
{
    // Architecture-safety: different resolved drvs (e.g.
    // x86 vs aarch64) are distinct keys and never coalesce.
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a, b;
    auto ra = reg->startOrAttach(auth, key("x86"), a.logSink(), a.resultSink(), {});
    auto rb = reg->startOrAttach(auth, key("aarch64"), b.logSink(), b.resultSink(), {});

    ASSERT_TRUE(ra);
    ASSERT_TRUE(rb);
    EXPECT_TRUE(ra->started);
    EXPECT_TRUE(rb->started);
    EXPECT_TRUE(reg->isLive(key("x86")));
    EXPECT_TRUE(reg->isLive(key("aarch64")));
}

/* ------------------------------------------------------------------------ *
 * Log fan-out (broadcaster)
 * ------------------------------------------------------------------------ */

TEST(BuildRegistry, logFansOutToAllSubscribers)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a, b;
    reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {});
    reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), {});

    reg->log(key("k"), "line1\n");
    reg->log(key("k"), "line2\n");

    EXPECT_EQ(a.liveData(), (std::vector<std::string>{"line1\n", "line2\n"}));
    EXPECT_EQ(b.liveData(), (std::vector<std::string>{"line1\n", "line2\n"}));
}

/* ------------------------------------------------------------------------ *
 * Late-join replay
 * ------------------------------------------------------------------------ */

TEST(BuildRegistry, lateJoinerReplaysThenFollowsLiveTail)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a;
    reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {});
    reg->log(key("k"), "early1\n");
    reg->log(key("k"), "early2\n");

    // Late joiner: replays the buffer (replayed=true), then follows the live
    // tail with nothing missed or duplicated at the handover.
    Recorder b;
    SubscribeOptions opts{.replayWanted = true};
    auto rb = reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), opts);
    ASSERT_TRUE(rb);
    EXPECT_TRUE(rb->deduplicated);
    EXPECT_EQ(b.replayedData(), (std::vector<std::string>{"early1\n", "early2\n"}));
    EXPECT_TRUE(b.liveData().empty());

    reg->log(key("k"), "live3\n");
    EXPECT_EQ(b.liveData(), (std::vector<std::string>{"live3\n"}));
    // The originator never sees replayed frames, and sees every live frame once.
    EXPECT_TRUE(a.replayedData().empty());
    EXPECT_EQ(a.liveData(), (std::vector<std::string>{"early1\n", "early2\n", "live3\n"}));
}

TEST(BuildRegistry, replayCanBeDeclined)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a;
    reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {});
    reg->log(key("k"), "early\n");

    Recorder b;
    SubscribeOptions opts{.replayWanted = false};
    reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), opts);
    EXPECT_TRUE(b.replayedData().empty());
    EXPECT_TRUE(b.liveData().empty());
}

TEST(BuildRegistry, replayBufferTruncatesWithMarker)
{
    // head+tail cap with an explicit truncation marker once the tail evicts.
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy, ReplayBufferCaps{.headCap = 8, .tailCap = 8});
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a;
    reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {});
    for (int i = 0; i < 20; ++i)
        reg->log(key("k"), "abcd\n"); // 5 bytes each

    Recorder b;
    auto rb = reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), {});
    ASSERT_TRUE(rb);
    auto replayed = b.replayedData();
    bool sawMarker = false;
    for (auto & f : replayed)
        if (f.find("truncated") != std::string::npos)
            sawMarker = true;
    EXPECT_TRUE(sawMarker);
    // The replayed snapshot is bounded — far fewer than the 20 frames emitted.
    EXPECT_LT(replayed.size(), size_t(20));
}

/* ------------------------------------------------------------------------ *
 * Result delivery + deduplicated stamping
 * ------------------------------------------------------------------------ */

TEST(BuildRegistry, finishDeliversToAllWithPerSubscriberDeduplicated)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a, b;
    reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {}); // originator
    reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), {}); // joiner

    BuildResult res = registrySuccessResult();
    res.builderId = "builder-7";
    reg->finish(key("k"), res);

    ASSERT_TRUE(a.result);
    ASSERT_TRUE(b.result);
    // Per-subscriber deduplicated stamp: originator false, joiner true.
    EXPECT_FALSE(a.result->deduplicated);
    EXPECT_TRUE(b.result->deduplicated);
    // builderId flows through unchanged to both.
    EXPECT_EQ(a.result->builderId, "builder-7");
    EXPECT_EQ(b.result->builderId, "builder-7");
    // Entry dropped after finish.
    EXPECT_FALSE(reg->isLive(key("k")));
}

TEST(BuildRegistry, transientFailureIsNotReusedForLaterArrivals)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a;
    reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {});

    // The build dies for a builder/infra reason (e.g. OOM kill), not because
    // of the derivation: a transient-class failure.
    BuildResult res;
    res.inner = BuildResult::Failure{{
        .status = BuildResult::Failure::MiscFailure,
        .msg = HintFmt("killed"),
    }};
    res.failureClass = BuildResult::FailureClass::ResourceExhausted;
    ASSERT_TRUE(res.failureIsTransient());
    reg->finish(key("k"), res);

    // The attached subscriber gets the failure (it observed this execution)...
    ASSERT_TRUE(a.result);
    EXPECT_EQ(a.result->failureClass, BuildResult::FailureClass::ResourceExhausted);

    // ...but the result is not retained: the key is no longer live, and a
    // later arrival is a MISS that starts a fresh build rather than being
    // handed the transient failure.
    EXPECT_FALSE(reg->isLive(key("k")));
    Recorder b;
    auto rb = reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), {});
    ASSERT_TRUE(rb);
    EXPECT_FALSE(rb->deduplicated) << "later arrival must start fresh, not attach to anything";
    EXPECT_FALSE(b.result) << "the stale transient failure must not be replayed";
}

TEST(BuildRegistry, finishIsIdempotentAfterCancel)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    bool cancelled = false;
    Recorder a;
    auto ra = reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {}, [&] { cancelled = true; });
    ASSERT_TRUE(ra);
    reg->unsubscribe(ra->id, DetachReason::Hup); // refcount → 0, no root → cancel
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(reg->isLive(key("k")));

    // A late finish for the dropped key is a harmless no-op.
    reg->finish(key("k"), registrySuccessResult());
    EXPECT_FALSE(a.result);
}

/* ------------------------------------------------------------------------ *
 * Refcounted cancellation matrix
 * ------------------------------------------------------------------------ */

TEST(BuildRegistry, cancel_C_a_continuesWhenAnotherSubscriberRemains)
{
    // Two attached, kill #1 (the originator) → build continues, #2 completes;
    // no originator privilege.
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    bool cancelled = false;
    Recorder a, b;
    auto ra = reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {}, [&] { cancelled = true; });
    auto rb = reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), {});
    ASSERT_TRUE(ra);
    ASSERT_TRUE(rb);

    reg->unsubscribe(ra->id, DetachReason::Hup); // originator leaves
    EXPECT_FALSE(cancelled);
    EXPECT_TRUE(reg->isLive(key("k")));

    reg->finish(key("k"), registrySuccessResult());
    ASSERT_TRUE(b.result);  // #2 still gets the result
    EXPECT_FALSE(a.result); // #1 already detached
}

TEST(BuildRegistry, cancel_C_b_cancelsWhenLastSubscriberLeavesNoRoot)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    bool cancelled = false;
    Recorder a;
    auto ra = reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {}, [&] { cancelled = true; });
    ASSERT_TRUE(ra);
    reg->unsubscribe(ra->id, DetachReason::Hup);
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(reg->isLive(key("k")));
}

TEST(BuildRegistry, cancel_C_c_explicitRootKeepsBuildAlivePastRefcountZero)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    bool cancelled = false;
    Recorder a;
    SubscribeOptions opts{.explicitRoot = true};
    auto ra = reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), opts, [&] { cancelled = true; });
    ASSERT_TRUE(ra);
    reg->unsubscribe(ra->id, DetachReason::Hup); // refcount → 0 but rooted
    EXPECT_FALSE(cancelled);
    EXPECT_TRUE(reg->isLive(key("k")));             // continues to completion
    reg->finish(key("k"), registrySuccessResult()); // delivered to nobody, drops entry
    EXPECT_FALSE(reg->isLive(key("k")));
}

TEST(BuildRegistry, cancel_lateJoinAfterCancelIsAFreshBuild)
{
    // Models the race-free property: once the last subscriber leaves and the
    // build is cancelled+dropped, a subsequent request is a fresh MISS, never
    // an attach to a dead build.
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a;
    auto ra = reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {}, [] {});
    ASSERT_TRUE(ra);
    reg->unsubscribe(ra->id, DetachReason::Hup);
    EXPECT_FALSE(reg->isLive(key("k")));

    Recorder b;
    auto rb = reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), {}, [] {});
    ASSERT_TRUE(rb);
    EXPECT_TRUE(rb->started);
    EXPECT_FALSE(rb->deduplicated);
}

TEST(BuildRegistry, cancel_C_f_activeCancelScopedToCancellerWhenOthersRemain)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    bool cancelled = false;
    Recorder a, b;
    auto ra = reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {}, [&] { cancelled = true; });
    auto rb = reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), {});
    ASSERT_TRUE(ra);
    ASSERT_TRUE(rb);

    reg->unsubscribe(ra->id, DetachReason::Cancel); // active cancel by #1
    EXPECT_FALSE(cancelled);                        // build unaffected — #2 still attached
    EXPECT_TRUE(reg->isLive(key("k")));
    reg->finish(key("k"), registrySuccessResult());
    EXPECT_TRUE(b.result);
}

/* ------------------------------------------------------------------------ *
 * keep-failed OR
 * ------------------------------------------------------------------------ */

TEST(BuildRegistry, keepFailedIsLogicalOr)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a, b;
    SubscribeOptions wants{.keepFailed = true};
    reg->startOrAttach(auth, key("k"), a.logSink(), a.resultSink(), {}); // no keep-failed
    EXPECT_FALSE(reg->keepFailedRequested(key("k")));
    reg->startOrAttach(auth, key("k"), b.logSink(), b.resultSink(), wants); // one wants it
    EXPECT_TRUE(reg->keepFailedRequested(key("k")));
}

/* ------------------------------------------------------------------------ *
 * Authorization before registry (no existence oracle)
 * ------------------------------------------------------------------------ */

TEST(BuildRegistry, authorizeBeforeRegistryUniformDenial)
{
    TrustedOnlyPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);

    BuildAuth trusted{.identity = "root", .trusted = true};
    BuildAuth untrusted{.identity = "mallory", .trusted = false};

    // A trusted user starts the in-flight "secret" build.
    Recorder t;
    auto rt = reg->startOrAttach(trusted, key("secret"), t.logSink(), t.resultSink(), {});
    ASSERT_TRUE(rt);
    EXPECT_TRUE(reg->isLive(key("secret")));

    // The untrusted identity is denied — identically whether the key is in
    // flight ("secret", which exists) or not ("phantom", which does not): both
    // return nullopt, so membership is not revealed.
    Recorder m1, m2;
    EXPECT_FALSE(reg->startOrAttach(untrusted, key("secret"), m1.logSink(), m1.resultSink(), {}));
    EXPECT_FALSE(reg->startOrAttach(untrusted, key("phantom"), m2.logSink(), m2.resultSink(), {}));
    // No frames leaked to the denied caller.
    EXPECT_TRUE(m1.frames.empty());

    // queryActive is authorization-filtered: the untrusted identity sees none.
    EXPECT_TRUE(reg->queryActive(untrusted).empty());
}

/* The coordinator's shipped policy: only its own identity may build, attach,
   or observe — the single-user gate enforced by the registry itself rather
   than only by the transport's same-uid accept check. */
TEST(BuildRegistry, singleIdentityPolicyDeniesForeignIdentityUniformly)
{
    SingleIdentityAuthPolicy policy{"1000"};
    auto reg = makeInMemoryBuildRegistry(policy);

    BuildAuth own{.identity = "1000", .trusted = true};
    // Trusted but *foreign*: trust does not bypass the identity boundary.
    BuildAuth foreign{.identity = "1001", .trusted = true};

    // The owning identity can build...
    Recorder t;
    auto rt = reg->startOrAttach(own, key("mine"), t.logSink(), t.resultSink(), {});
    ASSERT_TRUE(rt);
    EXPECT_TRUE(rt->started);
    EXPECT_TRUE(reg->isLive(key("mine")));

    // ...and attach (same-identity dedup still works).
    Recorder t2;
    auto rt2 = reg->startOrAttach(own, key("mine"), t2.logSink(), t2.resultSink(), {});
    ASSERT_TRUE(rt2);
    EXPECT_TRUE(rt2->deduplicated);

    // A foreign identity is denied uniformly — for a key that exists and one
    // that does not — so it can neither attach to another identity's build
    // nor probe for its existence.
    Recorder f1, f2;
    EXPECT_FALSE(reg->startOrAttach(foreign, key("mine"), f1.logSink(), f1.resultSink(), {}));
    EXPECT_FALSE(reg->startOrAttach(foreign, key("phantom"), f2.logSink(), f2.resultSink(), {}));
    EXPECT_TRUE(f1.frames.empty());

    // And it observes nothing.
    EXPECT_TRUE(reg->queryActive(foreign).empty());
    EXPECT_FALSE(reg->queryActive(own).empty());
}

/* ------------------------------------------------------------------------ *
 * Introspection
 * ------------------------------------------------------------------------ */

TEST(BuildRegistry, queryActiveReportsLiveBuilds)
{
    AllowAllAuthPolicy policy;
    auto reg = makeInMemoryBuildRegistry(policy);
    BuildAuth auth{.identity = "u", .trusted = true};

    Recorder a, b;
    SubscribeOptions rooted{.explicitRoot = true};
    reg->startOrAttach(auth, key("k1"), a.logSink(), a.resultSink(), rooted);
    reg->startOrAttach(auth, key("k1"), b.logSink(), b.resultSink(), {});
    reg->log(key("k1"), "hello\n");

    auto active = reg->queryActive(auth);
    ASSERT_EQ(active.size(), size_t(1));
    EXPECT_EQ(active[0].key, key("k1"));
    EXPECT_EQ(active[0].subscriberCount, size_t(2));
    EXPECT_EQ(active[0].logBytes, uint64_t(6));
    EXPECT_TRUE(active[0].rooted);
}

} // namespace nix
