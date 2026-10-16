#include "dlssnr/backend/NrInlineAutoSpin.h"
#include "dlssnr/NrFrameDiag.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace DlssNr::Backend::InlineAutoSpin;

namespace
{
WorkloadSignature DefaultWorkload()
{
    WorkloadSignature sig {};
    sig.sessionId = 0x100;
    sig.gameQueueId = 0x200;
    sig.swapchainId = 0x300;
    sig.width = 1920;
    sig.height = 1080;
    sig.format = 10;
    sig.passes = 1;
    sig.networkFlags = 0x0F;
    sig.inlineReuse = false;
    sig.watchSlices = 0;
    sig.watchStartDelay = 0;
    return sig;
}

RuntimeConfigInput DefaultConfig()
{
    RuntimeConfigInput cfg {};
    cfg.inlineEnabled = true;
    cfg.isMochizuki = true;
    cfg.gpuHandoff = true;
    cfg.autoSpinEnabled = true;
    cfg.adaptiveWaitBudget = false;
    cfg.fixedPollDelay = std::nullopt;
    cfg.fixedSlices = std::nullopt;
    return cfg;
}

SampleInput MakeSample(const Controller& ctrl, double presentMs, double gpuWaitMs = 0.8, double gpuSpanMs = 3.2)
{
    SampleInput s {};
    s.epoch = ctrl.CurrentEpoch();
    s.candidate = ctrl.CurrentCandidate();
    s.presentIntervalMs = presentMs;
    s.gpuWaitMs = gpuWaitMs;
    s.gpuFrameSpanMs = gpuSpanMs;
    s.inlineSubmitted = true;
    s.captureMissed = false;
    s.spinCapped = false;
    s.historyReset = false;
    s.replayed = false;
    return s;
}

void FeedSteadyWindow(Controller& ctrl, uint32_t frames, double presentMs, double gpuWaitMs = 0.8, double gpuSpanMs = 3.2)
{
    for (uint32_t i = 0; i < frames; ++i)
    {
        ctrl.OnSample(MakeSample(ctrl, presentMs, gpuWaitMs, gpuSpanMs));
    }
}

void TestWhitelistPriorityAndFiltering()
{
    // Verify exact priority order of the 9-candidate whitelist.
    assert(kWhitelist.size() == 9);
    assert((kWhitelist[0] == Candidate{16, 64}));
    assert((kWhitelist[1] == Candidate{0, 256}));
    assert((kWhitelist[2] == Candidate{0, 1024}));
    assert((kWhitelist[3] == Candidate{0, 64}));
    assert((kWhitelist[4] == Candidate{16, 256}));
    assert((kWhitelist[5] == Candidate{16, 1024}));
    assert((kWhitelist[6] == Candidate{64, 64}));
    assert((kWhitelist[7] == Candidate{64, 256}));
    assert((kWhitelist[8] == Candidate{64, 1024}));

    // Both Auto -> baseline (16,64) + 8 explore candidates
    {
        Controller ctrl;
        ctrl.UpdateConfigAndWorkload(DefaultConfig(), DefaultWorkload());
        const auto snap = ctrl.GetStatus();
        assert(snap.state == State::Stabilizing);
        assert(snap.candidateTotal == 8);
        assert((snap.activeCandidate == Candidate{16, 64}));
    }

    // Fixed PollDelay = 0, Slices = Auto -> baseline (0,64) + 2 explore candidates: (0,256), (0,1024)
    {
        Controller ctrl;
        auto cfg = DefaultConfig();
        cfg.fixedPollDelay = 0;
        ctrl.UpdateConfigAndWorkload(cfg, DefaultWorkload());
        const auto snap = ctrl.GetStatus();
        assert(snap.state == State::Stabilizing);
        assert(snap.candidateTotal == 2);
        assert((snap.activeCandidate == Candidate{0, 64}));
    }

    // Fixed Slices = 0 (normalized to 64), PollDelay = Auto -> baseline (16,64) + 2 explore candidates: (0,64), (64,64)
    {
        Controller ctrl;
        auto cfg = DefaultConfig();
        cfg.fixedSlices = 0; // normalized to 64
        ctrl.UpdateConfigAndWorkload(cfg, DefaultWorkload());
        const auto snap = ctrl.GetStatus();
        assert(snap.state == State::Stabilizing);
        assert(snap.candidateTotal == 2);
        assert((snap.activeCandidate == Candidate{16, 64}));
    }
}

void TestInactiveAndSuspendReasons()
{
    Controller ctrl;
    const auto wl = DefaultWorkload();

    // Non-Mochizuki -> Inactive
    {
        auto cfg = DefaultConfig();
        cfg.isMochizuki = false;
        ctrl.UpdateConfigAndWorkload(cfg, wl);
        assert(ctrl.GetStatus().state == State::Inactive);
        assert(ctrl.GetStatus().reason == SuspendReason::NotMochizuki);
    }

    // inlineEnabled = false -> Inactive
    {
        auto cfg = DefaultConfig();
        cfg.inlineEnabled = false;
        ctrl.UpdateConfigAndWorkload(cfg, wl);
        assert(ctrl.GetStatus().state == State::Inactive);
        assert(ctrl.GetStatus().reason == SuspendReason::InlineDisabled);
    }

    // gpuHandoff = false -> Inactive
    {
        auto cfg = DefaultConfig();
        cfg.gpuHandoff = false;
        ctrl.UpdateConfigAndWorkload(cfg, wl);
        assert(ctrl.GetStatus().state == State::Inactive);
        assert(ctrl.GetStatus().reason == SuspendReason::GpuHandoffDisabled);
    }

    // adaptiveWaitBudget = true -> Suspended(AdaptiveWaitBudgetActive)
    {
        auto cfg = DefaultConfig();
        cfg.adaptiveWaitBudget = true;
        ctrl.UpdateConfigAndWorkload(cfg, wl);
        const auto snap = ctrl.GetStatus();
        assert(snap.state == State::Suspended);
        assert(snap.reason == SuspendReason::AdaptiveWaitBudgetActive);
        assert((snap.activeCandidate == Candidate{16, 64}));
    }

    // autoSpinEnabled = false -> Suspended(AutoSpinDisabled), respects fixed axis or fallback (16, 64)
    {
        auto cfg = DefaultConfig();
        cfg.autoSpinEnabled = false;
        cfg.fixedPollDelay = 64;
        ctrl.UpdateConfigAndWorkload(cfg, wl);
        const auto snap = ctrl.GetStatus();
        assert(snap.state == State::Suspended);
        assert(snap.reason == SuspendReason::AutoSpinDisabled);
        assert((snap.activeCandidate == Candidate{64, 64}));
    }

    // Both axes fixed -> Suspended(BothAxesFixed)
    {
        auto cfg = DefaultConfig();
        cfg.fixedPollDelay = 0;
        cfg.fixedSlices = 1024;
        ctrl.UpdateConfigAndWorkload(cfg, wl);
        const auto snap = ctrl.GetStatus();
        assert(snap.state == State::Suspended);
        assert(snap.reason == SuspendReason::BothAxesFixed);
        assert((snap.activeCandidate == Candidate{0, 1024}));
    }
}

void TestEpochFilteringAndMixedWindowRejection()
{
    Controller ctrl;
    ctrl.UpdateConfigAndWorkload(DefaultConfig(), DefaultWorkload());
    assert(ctrl.GetStatus().state == State::Stabilizing);

    // Feed 89 frames in Stabilizing (@ 20ms so 90 frames = 1800ms >= 1500ms), plus stale epoch and replayed samples.
    const uint64_t currentEpoch = ctrl.CurrentEpoch();
    for (int i = 0; i < 89; ++i)
    {
        // Stale epoch sample should be completely ignored.
        SampleInput stale = MakeSample(ctrl, 500.0);
        stale.epoch = currentEpoch - 1;
        ctrl.OnSample(stale);

        // Replayed sample should be ignored.
        SampleInput replayed = MakeSample(ctrl, 50.0);
        replayed.replayed = true;
        ctrl.OnSample(replayed);

        ctrl.OnSample(MakeSample(ctrl, 20.0));
    }
    assert(ctrl.GetStatus().state == State::Stabilizing);

    // 90th valid frame transitions Stabilizing -> Exploring (InitialBaselineA).
    ctrl.OnSample(MakeSample(ctrl, 20.0));
    assert(ctrl.GetStatus().state == State::Exploring);
    assert(ctrl.GetStatus().phase == PhaseKind::InitialBaselineA);
}

void TestFullSearchConfirmAndLockByPresentImprovement()
{
    Controller ctrl;
    auto cfg = DefaultConfig();
    cfg.fixedPollDelay = 0; // baseline=(0,64), explore=[(0,256), (0,1024)]
    ctrl.UpdateConfigAndWorkload(cfg, DefaultWorkload());
    assert(ctrl.GetStatus().state == State::Stabilizing);

    // Stabilizing: 90 frames @ 20.0 ms (1800 ms >= 1500 ms)
    FeedSteadyWindow(ctrl, Controller::kStabilizeWarmupFrames, 20.0, 1.0, 3.0);
    assert(ctrl.GetStatus().state == State::Exploring);
    assert(ctrl.GetStatus().phase == PhaseKind::InitialBaselineA);
    assert((ctrl.CurrentCandidate() == Candidate{0, 64}));

    // InitialBaselineA: 90 measure frames @ 10.0 ms (no cooldown since candidate didn't change)
    FeedSteadyWindow(ctrl, Controller::kMeasureWindowFrames, 10.0, 1.0, 3.0);
    assert(ctrl.GetStatus().phase == PhaseKind::ExploreCandidateB);
    assert((ctrl.CurrentCandidate() == Candidate{0, 256}));

    // ExploreCandidateB #0 (0, 256): 24 cooldown + 90 measure @ 9.5 ms (5% faster than 10.0 ms)
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 9.5, 0.9, 2.9);
    assert(ctrl.GetStatus().phase == PhaseKind::ExplorePostBaselineA);
    assert((ctrl.CurrentCandidate() == Candidate{0, 64}));

    // ExplorePostBaselineA: 24 cooldown + 90 measure @ 10.0 ms (0% drift)
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0, 1.0, 3.0);
    // Next candidate #1 (0, 1024) reuses post-baseline as pre-baseline, entering ExploreCandidateB directly.
    assert(ctrl.GetStatus().phase == PhaseKind::ExploreCandidateB);
    assert((ctrl.CurrentCandidate() == Candidate{0, 1024}));

    // ExploreCandidateB #1 (0, 1024): 24 cooldown + 90 measure @ 9.9 ms (only 1% faster, loses to (0,256)'s 5%)
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 9.9, 0.95, 2.95);
    assert(ctrl.GetStatus().phase == PhaseKind::ExplorePostBaselineA);

    // ExplorePostBaselineA for #1: 24 cooldown + 90 measure @ 10.0 ms
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0, 1.0, 3.0);

    // All candidates explored; (0,256) beat baseline by 5% (>= 1.5%), so enter Confirming!
    assert(ctrl.GetStatus().state == State::Confirming);
    assert(ctrl.ShouldMeasureGpuStamps());
    assert(ctrl.GetStatus().phase == PhaseKind::ConfirmWinnerB);
    assert((ctrl.CurrentCandidate() == Candidate{0, 256}));

    // ConfirmWinnerB: 24 cooldown + 180 measure @ 9.5 ms
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kConfirmWindowFrames, 9.5, 0.9, 2.9);
    assert(ctrl.GetStatus().state == State::Confirming);
    assert(ctrl.GetStatus().phase == PhaseKind::ConfirmBaselineA);
    assert((ctrl.CurrentCandidate() == Candidate{0, 64}));

    // ConfirmBaselineA: 24 cooldown + 180 measure @ 10.0 ms
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kConfirmWindowFrames, 10.0, 1.0, 3.0);
    assert(ctrl.GetStatus().state == State::Locked);
    assert(ctrl.ShouldMeasure());
    assert(!ctrl.ShouldMeasureGpuStamps());
    assert((ctrl.CurrentCandidate() == Candidate{0, 256}));
    assert((ctrl.GetStatus().bestCandidate == Candidate{0, 256}));
}

void TestGpuWaitSecondaryTieBreakWin()
{
    Controller ctrl;
    auto cfg = DefaultConfig();
    cfg.fixedPollDelay = 16; // baseline=(16,64), explore=[(16,256), (16,1024)]
    ctrl.UpdateConfigAndWorkload(cfg, DefaultWorkload());

    // Stabilizing: 90 frames @ 20.0 ms
    FeedSteadyWindow(ctrl, Controller::kStabilizeWarmupFrames, 20.0, 1.0, 3.0);

    // InitialBaselineA: 90 @ 10.0 ms, gpuWait = 1.0 ms
    FeedSteadyWindow(ctrl, Controller::kMeasureWindowFrames, 10.0, 1.0, 3.0);

    // ExploreCandidateB #0 (16, 256): Present is 10.0 ms (0% change, within 0.5% noise band),
    // but gpuWait drops from 1.0 ms to 0.70 ms (-0.30 ms >= 0.25 ms).
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0, 0.70, 2.70);
    // ExplorePostBaselineA:
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0, 1.0, 3.0);

    // ExploreCandidateB #1 (16, 1024): Present 10.0 ms, gpuWait = 0.90 ms (-0.10 ms < 0.25 ms)
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0, 0.90, 2.90);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0, 1.0, 3.0);

    // Should enter Confirming for Candidate (16, 256)
    assert(ctrl.GetStatus().state == State::Confirming);
    assert((ctrl.CurrentCandidate() == Candidate{16, 256}));
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kConfirmWindowFrames, 10.0, 0.70, 2.70);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kConfirmWindowFrames, 10.0, 1.0, 3.0);

    assert(ctrl.GetStatus().state == State::Locked);
    assert((ctrl.CurrentCandidate() == Candidate{16, 256}));
}

void TestBaselineDriftRejection()
{
    Controller ctrl;
    auto cfg = DefaultConfig();
    cfg.fixedPollDelay = 0; // baseline=(0,64), explore=[(0,256), (0,1024)]
    ctrl.UpdateConfigAndWorkload(cfg, DefaultWorkload());

    FeedSteadyWindow(ctrl, Controller::kStabilizeWarmupFrames, 20.0);
    // InitialBaselineA @ 10.0 ms
    FeedSteadyWindow(ctrl, Controller::kMeasureWindowFrames, 10.0);
    // Candidate #0 (0,256) looks great @ 8.5 ms
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 8.5);
    // But ExplorePostBaselineA drifted to 11.5 ms (+15% > 10% drift threshold)!
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 11.5);

    // Drift triggers 1 retry of ExplorePreBaselineA for Candidate #0.
    assert(ctrl.GetStatus().state == State::Exploring);
    assert(ctrl.GetStatus().phase == PhaseKind::ExplorePreBaselineA);
    assert((ctrl.CurrentCandidate() == Candidate{0, 64}));

    // On retry: PreBaselineA @ 10.0 ms, Candidate #0 @ 8.5 ms, PostBaselineA drifts again @ 11.5 ms!
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 8.5);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 11.5);

    // Now retry is exhausted; Candidate #0 is skipped and Candidate #1 starts with ExplorePreBaselineA.
    assert(ctrl.GetStatus().phase == PhaseKind::ExplorePreBaselineA);
    assert(ctrl.GetStatus().candidateCursor == 1);

    // Finish Candidate #1 with no improvement @ 10.0 ms
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0);

    // Because Candidate #0 was rejected for baseline drift, baseline (0,64) locks directly.
    assert(ctrl.GetStatus().state == State::Locked);
    assert((ctrl.CurrentCandidate() == Candidate{0, 64}));
}

void TestImmediateAbortOnCapAndConfirmP95Rejection()
{
    Controller ctrl;
    auto cfg = DefaultConfig();
    cfg.fixedPollDelay = 0; // baseline=(0,64), explore=[(0,256), (0,1024)]
    ctrl.UpdateConfigAndWorkload(cfg, DefaultWorkload());

    FeedSteadyWindow(ctrl, Controller::kStabilizeWarmupFrames, 20.0);
    // InitialBaselineA @ 10.0 ms
    FeedSteadyWindow(ctrl, Controller::kMeasureWindowFrames, 10.0);

    // Candidate #0 (0,256): 24 cooldown frames, then a spinCapped occurs on the 1st measurement frame!
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames, 10.0);
    {
        SampleInput bad = MakeSample(ctrl, 10.0);
        bad.spinCapped = true;
        ctrl.OnSample(bad);
    }
    // Candidate #0 must immediately abort and move to Candidate #1's ExplorePreBaselineA!
    assert(ctrl.GetStatus().phase == PhaseKind::ExplorePreBaselineA);
    assert(ctrl.GetStatus().candidateCursor == 1);
    assert((ctrl.CurrentCandidate() == Candidate{0, 64}));

    // ExplorePreBaselineA for Candidate #1 (0,1024) @ 10.0 ms
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0);
    assert(ctrl.GetStatus().phase == PhaseKind::ExploreCandidateB);
    assert((ctrl.CurrentCandidate() == Candidate{0, 1024}));

    // Candidate #1 (0,1024) wins in Exploring @ 9.5 ms, PostBaselineA @ 10.0 ms
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 9.5);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0);

    // Enters Confirming for Candidate #1 (0,1024)
    assert(ctrl.GetStatus().state == State::Confirming);
    assert(ctrl.GetStatus().phase == PhaseKind::ConfirmWinnerB);
    assert((ctrl.CurrentCandidate() == Candidate{0, 1024}));

    // ConfirmWinnerB: 24 cooldown, then 165 frames @ 9.5 ms, and 15 frames spike to 12.0 ms so p95 = 12.0 ms!
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames, 9.5);
    FeedSteadyWindow(ctrl, 165, 9.5);
    FeedSteadyWindow(ctrl, 15, 12.0);

    // ConfirmBaselineA @ 10.0 ms (p95 = 10.0 ms, so Challenger p95 12.0 > 10.3 is rejected!)
    assert(ctrl.GetStatus().phase == PhaseKind::ConfirmBaselineA);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kConfirmWindowFrames, 10.0);

    // Confirmation rejects ChallengerB due to p95 regression and locks BaselineA (0,64)!
    assert(ctrl.GetStatus().state == State::Locked);
    assert((ctrl.CurrentCandidate() == Candidate{0, 64}));
}

void TestWorkloadInvalidationAndSoftSceneDriftReeval()
{
    Controller ctrl;
    auto cfg = DefaultConfig();
    cfg.fixedPollDelay = 0; // baseline=(0,64), explore=[(0,256), (0,1024)]
    auto wl = DefaultWorkload();
    ctrl.UpdateConfigAndWorkload(cfg, wl);

    // Fast-path to Locked on baseline (0,64)
    FeedSteadyWindow(ctrl, Controller::kStabilizeWarmupFrames, 20.0);
    FeedSteadyWindow(ctrl, Controller::kMeasureWindowFrames, 10.0);
    for (int c = 0; c < 2; ++c)
    {
        FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.2);
        FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0);
    }
    assert(ctrl.GetStatus().state == State::Locked);

    // UI status query with empty WorkloadSignature{} must NOT reset Locked state!
    ctrl.UpdateConfigAndWorkload(cfg, WorkloadSignature{});
    assert(ctrl.GetStatus().state == State::Locked);

    // Soft scene drift while Locked:
    // Locked cooldown is 600 frames. During cooldown, drift does not trigger re-evaluation.
    FeedSteadyWindow(ctrl, Controller::kReevalCooldownFrames, 15.0);
    assert(ctrl.GetStatus().state == State::Locked);

    // Now feed kSceneChangeHoldFrames (60) frames with >25% Present shift (15.0 ms vs 10.0 ms).
    FeedSteadyWindow(ctrl, Controller::kSceneChangeHoldFrames, 15.0);
    assert(ctrl.GetStatus().state == State::Stabilizing);

}

void TestRetryBudgetDoesNotSkipLaterCandidates()
{
    Controller ctrl;
    auto cfg = DefaultConfig();
    cfg.fixedPollDelay = 0; // baseline=(0,64), explore=[(0,256), (0,1024)]
    ctrl.UpdateConfigAndWorkload(cfg, DefaultWorkload());

    FeedSteadyWindow(ctrl, Controller::kStabilizeWarmupFrames, 20.0);
    FeedSteadyWindow(ctrl, Controller::kMeasureWindowFrames, 10.0);
    // Candidate 0 looks better, then the post-baseline drifts once. That spends the retry.
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 8.5);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 11.5);
    assert(ctrl.GetStatus().phase == PhaseKind::ExplorePreBaselineA);

    // The retried pre-baseline is stable, then candidate 0 caps and is abandoned.
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames + Controller::kMeasureWindowFrames, 10.0);
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames, 10.0);
    {
        SampleInput bad = MakeSample(ctrl, 10.0);
        bad.spinCapped = true;
        ctrl.OnSample(bad);
    }
    assert(ctrl.GetStatus().phase == PhaseKind::ExplorePreBaselineA);
    assert(ctrl.GetStatus().candidateCursor == 1);

    // One invalid window is the next candidate's own retry, not the end of the search.
    FeedSteadyWindow(ctrl, Controller::kSwitchCooldownFrames, 10.0);
    FeedSteadyWindow(ctrl, Controller::kMeasureWindowFrames - 1, 10.0);
    {
        SampleInput missed = MakeSample(ctrl, 10.0);
        missed.captureMissed = true;
        ctrl.OnSample(missed);
    }
    assert(ctrl.GetStatus().state == State::Exploring);
    assert(ctrl.GetStatus().phase == PhaseKind::ExplorePreBaselineA);
    assert(ctrl.GetStatus().candidateCursor == 1);
}

void TestIdentityFillInAndExtentHold()
{
    Controller ctrl;
    auto cfg = DefaultConfig();
    auto wl = DefaultWorkload();
    wl.swapchainId = 0;
    wl.gameQueueId = 0;
    ctrl.UpdateConfigAndWorkload(cfg, wl);
    FeedSteadyWindow(ctrl, 10, 16.0);
    const uint64_t epoch = ctrl.CurrentEpoch();
    const uint32_t warmed = ctrl.GetStatus().phaseProgressFrames;
    assert(ctrl.GetStatus().state == State::Stabilizing);
    assert(warmed == 10);

    wl.swapchainId = 0x300;
    wl.gameQueueId = 0x200;
    ctrl.UpdateConfigAndWorkload(cfg, wl);
    assert(ctrl.CurrentEpoch() == epoch);
    assert(ctrl.GetStatus().phaseProgressFrames == warmed);

    wl.width = 1280;
    wl.height = 720;
    for (uint32_t i = 0; i < Controller::kStabilizeWarmupFrames - 1; ++i)
        ctrl.UpdateConfigAndWorkload(cfg, wl);
    assert(ctrl.CurrentEpoch() == epoch);
    assert(ctrl.GetStatus().state == State::Stabilizing);

    wl.width = 1920;
    wl.height = 1080;
    ctrl.UpdateConfigAndWorkload(cfg, wl);
    assert(ctrl.CurrentEpoch() == epoch);

    assert(ctrl.ShouldMeasure());
    assert(!ctrl.ShouldMeasureGpuStamps());

    wl.width = 2560;
    wl.height = 1440;
    for (uint32_t i = 0; i < Controller::kStabilizeWarmupFrames; ++i)
        ctrl.UpdateConfigAndWorkload(cfg, wl);
    assert(ctrl.GetStatus().state == State::Stabilizing);
    assert(ctrl.CurrentEpoch() != epoch);

    FeedSteadyWindow(ctrl, Controller::kStabilizeWarmupFrames, 20.0);
    assert(ctrl.GetStatus().state == State::Exploring);
    assert(ctrl.ShouldMeasure());
    assert(ctrl.ShouldMeasureGpuStamps());
}

void TestPresentSampleRingSwapchainFiltering()
{
    DlssNr::FrameDiag::PresentSampleRing ring;
    ring.SetActive(true);
    void* scA = reinterpret_cast<void*>(0x1000);
    void* scB = reinterpret_cast<void*>(0x2000);

    ring.OnPresent(scA);
    const uint64_t s1 = ring.CurrentSerial();
    assert(s1 == 1);

    // Secondary swapchain present within 250ms is ignored!
    ring.OnPresent(scB);
    assert(ring.CurrentSerial() == 1);
    assert(ring.CurrentSwapchainId() == reinterpret_cast<uintptr_t>(scA));
}
} // namespace

int main()
{
    TestWhitelistPriorityAndFiltering();
    TestInactiveAndSuspendReasons();
    TestEpochFilteringAndMixedWindowRejection();
    TestFullSearchConfirmAndLockByPresentImprovement();
    TestGpuWaitSecondaryTieBreakWin();
    TestBaselineDriftRejection();
    TestImmediateAbortOnCapAndConfirmP95Rejection();
    TestWorkloadInvalidationAndSoftSceneDriftReeval();
    TestRetryBudgetDoesNotSkipLaterCandidates();
    TestIdentityFillInAndExtentHold();
    TestPresentSampleRingSwapchainFiltering();
    std::printf("inline_auto_spin: PASS\n");
    return 0;
}
