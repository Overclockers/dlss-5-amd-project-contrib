#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace DlssNr::Backend
{
namespace InlineAutoSpin
{

struct Candidate
{
    int pollDelay = 16; // 0, 16, 64
    int slices = 64;    // 64 (0 in INI), 256, 1024

    int SlicesIniValue() const { return slices == 64 ? 0 : slices; }

    constexpr bool operator==(const Candidate& o) const
    {
        return pollDelay == o.pollDelay && slices == o.slices;
    }
    constexpr bool operator!=(const Candidate& o) const { return !(*this == o); }
};

inline constexpr std::array<Candidate, 9> kWhitelist {{
    { 16, 64 },   // 0: production baseline
    { 0, 256 },   // 1: priority (E33 sweet spot)
    { 0, 1024 },  // 2: priority (fine slice comparison)
    { 0, 64 },    // 3: priority (zero ALU poll delay control)
    { 16, 256 },  // 4: interaction check
    { 16, 1024 }, // 5: interaction check
    { 64, 64 },   // 6: extended poll delay
    { 64, 256 },  // 7: extended poll delay
    { 64, 1024 }, // 8: extended poll delay
}};

inline constexpr Candidate NormalizeCandidate(std::optional<int> fixedPollDelay, std::optional<int> fixedSlices,
                                              Candidate fallback = { 16, 64 })
{
    Candidate c = fallback;
    if (fixedPollDelay.has_value())
    {
        const int pd = *fixedPollDelay;
        if (pd == 0 || pd == 16 || pd == 64) c.pollDelay = pd;
    }
    if (fixedSlices.has_value())
    {
        const int sl = *fixedSlices;
        if (sl == 0 || sl == 64) c.slices = 64;
        else if (sl == 256 || sl == 1024) c.slices = sl;
    }
    return c;
}

enum class State : uint8_t
{
    Inactive = 0,
    Suspended,
    Stabilizing,
    Exploring,
    Confirming,
    Locked,
};

enum class SuspendReason : uint8_t
{
    None = 0,
    InlineDisabled,
    NotMochizuki,
    GpuHandoffDisabled,
    AutoSpinDisabled,
    BothAxesFixed,
    AdaptiveWaitBudgetActive,
};

enum class PhaseKind : uint8_t
{
    None = 0,
    InitialBaselineA,
    ExplorePreBaselineA,
    ExploreCandidateB,
    ExplorePostBaselineA,
    ConfirmWinnerB,
    ConfirmBaselineA,
};

struct WorkloadSignature
{
    uintptr_t sessionId = 0;
    uintptr_t gameQueueId = 0;
    uintptr_t swapchainId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    int32_t passes = 1;
    uint32_t networkFlags = 0;
    bool inlineReuse = false;
    int watchSlices = 0;
    int watchStartDelay = 0;

    bool IsValid() const { return sessionId != 0 && width > 0 && height > 0; }

    bool operator==(const WorkloadSignature& o) const
    {
        return sessionId == o.sessionId && gameQueueId == o.gameQueueId && swapchainId == o.swapchainId &&
               width == o.width && height == o.height && format == o.format && passes == o.passes &&
               networkFlags == o.networkFlags && inlineReuse == o.inlineReuse &&
               watchSlices == o.watchSlices && watchStartDelay == o.watchStartDelay;
    }
    bool operator!=(const WorkloadSignature& o) const { return !(*this == o); }
};

struct RuntimeConfigInput
{
    bool inlineEnabled = false;
    bool isMochizuki = false;
    bool gpuHandoff = true;
    bool autoSpinEnabled = true;
    bool adaptiveWaitBudget = false;
    std::optional<int> fixedPollDelay {}; // std::nullopt = Auto
    std::optional<int> fixedSlices {};    // std::nullopt = Auto (0 and 64 both map to 64)
};

struct SampleInput
{
    uint64_t epoch = 0;
    Candidate candidate {};
    double presentIntervalMs = 0.0; // > 0 when valid single-swapchain consecutive present interval
    double gpuWaitMs = -1.0;        // >= 0 when valid same-queue kGpuCapEnd -> kGpuResume
    double gpuFrameSpanMs = -1.0;   // >= 0 when valid same-queue kGpuCapBegin -> kGpuDone
    bool inlineSubmitted = true;
    bool captureMissed = false;     // handoffMiss / cap / timeout / abort
    bool spinCapped = false;        // game-wait capped or aborted
    bool historyReset = false;
    bool replayed = false;          // execution count > 1 or non-current candidate
};

struct WindowSummary
{
    bool valid = false;
    bool aborted = false;
    Candidate candidate {};
    uint32_t totalFrames = 0;
    uint32_t inlineFrames = 0;
    uint32_t missedFrames = 0;
    uint32_t cappedFrames = 0;
    uint32_t longFrames = 0;
    double medianPresentMs = 0.0;
    double p95PresentMs = 0.0;
    double medianGpuWaitMs = -1.0;
    double medianGpuSpanMs = -1.0;
    double coverage = 0.0;
};

struct StatusSnapshot
{
    State state = State::Inactive;
    SuspendReason reason = SuspendReason::None;
    PhaseKind phase = PhaseKind::None;
    Candidate activeCandidate { 16, 64 };
    Candidate baselineCandidate { 16, 64 };
    Candidate bestCandidate { 16, 64 };
    bool pollDelayFixed = false;
    bool slicesFixed = false;
    uint64_t epoch = 0;
    uint32_t phaseProgressFrames = 0;
    uint32_t phaseTargetFrames = 0;
    uint32_t candidateCursor = 0;
    uint32_t candidateTotal = 0;
    double baselineMedianPresentMs = 0.0;
    double baselineP95PresentMs = 0.0;
    double bestMedianPresentMs = 0.0;
    double bestP95PresentMs = 0.0;
};

class Controller
{
public:
    static constexpr uint32_t kStabilizeWarmupFrames = 90;
    static constexpr double kStabilizeWarmupMinMs = 1500.0;
    static constexpr uint32_t kSwitchCooldownFrames = 24;
    static constexpr uint32_t kMeasureWindowFrames = 90;
    static constexpr uint32_t kConfirmWindowFrames = 180;
    static constexpr uint32_t kMaxWindowRetry = 1;
    static constexpr uint32_t kSceneChangeHoldFrames = 60;
    static constexpr uint32_t kReevalCooldownFrames = 600;
    static constexpr double kMaxBaselineDriftRatio = 0.10;       // 10% A->B->A drift invalidates window
    static constexpr double kSceneChangeShiftRatio = 0.25;       // >25% sustained shift in Locked
    static constexpr double kMinCoverage = 0.98;                 // >=98% inline coverage
    static constexpr double kConfirmPresentGainRatio = 0.015;    // >=1.5% median present improvement
    static constexpr double kConfirmPresentGainAbsMs = 0.20;     // or >=0.20 ms median present improvement
    static constexpr double kConfirmGpuTiePresentRatio = 0.005;  // present within +/-0.5%
    static constexpr double kConfirmGpuWaitGainMs = 0.25;        // and GPU wait improves >=0.25 ms
    static constexpr double kMaxP95RegressionRatio = 1.03;       // p95_B <= p95_A * 1.03

    Controller() { RebuildSearchList(); }

    void UpdateConfigAndWorkload(const RuntimeConfigInput& cfg, const WorkloadSignature& sig)
    {
        const bool fixedChanged = (cfg.fixedPollDelay != cfg_.fixedPollDelay) || (cfg.fixedSlices != cfg_.fixedSlices);
        const bool modeChanged = (cfg.inlineEnabled != cfg_.inlineEnabled) ||
                                 (cfg.isMochizuki != cfg_.isMochizuki) ||
                                 (cfg.gpuHandoff != cfg_.gpuHandoff) ||
                                 (cfg.autoSpinEnabled != cfg_.autoSpinEnabled) ||
                                 (cfg.adaptiveWaitBudget != cfg_.adaptiveWaitBudget);
        cfg_ = cfg;

        if (fixedChanged) RebuildSearchList();

        // Check gate conditions first.
        if (!cfg_.inlineEnabled)
        {
            EnterInactiveOrSuspended(State::Inactive, SuspendReason::InlineDisabled);
            return;
        }
        if (!cfg_.isMochizuki)
        {
            EnterInactiveOrSuspended(State::Inactive, SuspendReason::NotMochizuki);
            return;
        }
        if (!cfg_.gpuHandoff)
        {
            EnterInactiveOrSuspended(State::Inactive, SuspendReason::GpuHandoffDisabled);
            return;
        }
        if (cfg_.adaptiveWaitBudget)
        {
            EnterInactiveOrSuspended(State::Suspended, SuspendReason::AdaptiveWaitBudgetActive);
            return;
        }
        if (!cfg_.autoSpinEnabled)
        {
            EnterInactiveOrSuspended(State::Suspended, SuspendReason::AutoSpinDisabled);
            return;
        }
        if (cfg_.fixedPollDelay.has_value() && cfg_.fixedSlices.has_value())
        {
            EnterInactiveOrSuspended(State::Suspended, SuspendReason::BothAxesFixed);
            return;
        }

        if (!sig.IsValid())
        {
            if (hasValidWorkload_ &&
                (fixedChanged || (modeChanged && (state_ == State::Inactive || state_ == State::Suspended))))
            {
                BeginStabilizing();
            }
            return;
        }

        // A zero queue or swapchain means that identity is not known yet. A later
        // real id fills the key. A new extent must hold for the warmup; dynamic
        // resolution would otherwise restart the search every frame.
        const WorkloadSignature resolved = NormalizeWorkload(sig);
        if (!hasValidWorkload_ || resolved != workload_ || fixedChanged ||
            (modeChanged && (state_ == State::Inactive || state_ == State::Suspended)))
        {
            workload_ = resolved;
            hasValidWorkload_ = true;
            BeginStabilizing();
        }
    }

    WorkloadSignature NormalizeWorkload(const WorkloadSignature& incoming)
    {
        WorkloadSignature sig = incoming;
        if (!hasValidWorkload_)
        {
            extentUnsettled_ = false;
            pendingExtentFrames_ = 0;
            return sig;
        }
        if (sig.swapchainId == 0)
            sig.swapchainId = workload_.swapchainId;
        else if (workload_.swapchainId == 0)
            workload_.swapchainId = sig.swapchainId;
        if (sig.gameQueueId == 0)
            sig.gameQueueId = workload_.gameQueueId;
        else if (workload_.gameQueueId == 0)
            workload_.gameQueueId = sig.gameQueueId;

        if (sig.width == workload_.width && sig.height == workload_.height)
        {
            pendingExtentFrames_ = 0;
            extentUnsettled_ = false;
            return sig;
        }
        if (sig.width == pendingWidth_ && sig.height == pendingHeight_)
            ++pendingExtentFrames_;
        else
        {
            pendingWidth_ = sig.width;
            pendingHeight_ = sig.height;
            pendingExtentFrames_ = 1;
        }
        if (pendingExtentFrames_ < kStabilizeWarmupFrames)
        {
            extentUnsettled_ = true;
            sig.width = workload_.width;
            sig.height = workload_.height;
            return sig;
        }
        extentUnsettled_ = false;
        pendingExtentFrames_ = 0;
        return sig;
    }

    void RequestReevaluate()
    {
        if (state_ == State::Inactive || state_ == State::Suspended) return;
        BeginStabilizing();
    }

    bool ShouldMeasure() const
    {
        return state_ == State::Stabilizing || state_ == State::Exploring ||
               state_ == State::Confirming || state_ == State::Locked;
    }

    uint64_t CurrentEpoch() const { return epoch_; }

    Candidate CurrentCandidate() const { return activeCandidate_; }

    StatusSnapshot GetStatus() const
    {
        StatusSnapshot s {};
        s.state = state_;
        s.reason = reason_;
        s.phase = phase_;
        s.activeCandidate = activeCandidate_;
        s.baselineCandidate = baselineCandidate_;
        s.bestCandidate = bestCandidate_;
        s.pollDelayFixed = cfg_.fixedPollDelay.has_value();
        s.slicesFixed = cfg_.fixedSlices.has_value();
        s.epoch = epoch_;
        if (state_ == State::Stabilizing)
        {
            s.phaseProgressFrames = stabilizeFrames_;
            s.phaseTargetFrames = kStabilizeWarmupFrames;
        }
        else if (state_ == State::Exploring || state_ == State::Confirming)
        {
            s.phaseProgressFrames = cooldownRemaining_ > 0
                ? 0
                : static_cast<uint32_t>(presentSamples_.size());
            s.phaseTargetFrames = windowTargetFrames_;
        }
        s.candidateCursor = exploreIndex_;
        s.candidateTotal = static_cast<uint32_t>(exploreCandidates_.size());
        s.baselineMedianPresentMs = refBaseline_.medianPresentMs;
        s.baselineP95PresentMs = refBaseline_.p95PresentMs;
        s.bestMedianPresentMs = bestSummary_.medianPresentMs;
        s.bestP95PresentMs = bestSummary_.p95PresentMs;
        return s;
    }

    void OnSample(const SampleInput& sample)
    {
        if (!ShouldMeasure()) return;
        if (sample.epoch != epoch_) return;
        if (extentUnsettled_) return;

        // Mixed-candidate or replay contamination check during active windows.
        if (sample.replayed || sample.candidate != activeCandidate_)
        {
            if (state_ == State::Exploring || state_ == State::Confirming)
            {
                ResetCurrentWindowWithCooldown();
            }
            return;
        }

        if (state_ == State::Stabilizing)
        {
            if (sample.presentIntervalMs > 0.0 && sample.inlineSubmitted && !sample.captureMissed && !sample.spinCapped)
            {
                ++stabilizeFrames_;
                stabilizeElapsedMs_ += sample.presentIntervalMs;
                if (stabilizeFrames_ >= kStabilizeWarmupFrames && stabilizeElapsedMs_ >= kStabilizeWarmupMinMs)
                {
                    StartInitialBaseline();
                }
            }
            return;
        }

        if (state_ == State::Locked)
        {
            ObserveLockedSample(sample);
            return;
        }

        // Exploring or Confirming
        if (cooldownRemaining_ > 0)
        {
            --cooldownRemaining_;
            return;
        }

        if (sample.presentIntervalMs <= 0.0)
        {
            return;
        }

        ++windowTotalFrames_;
        if (sample.inlineSubmitted) ++windowInlineFrames_;
        if (sample.captureMissed) ++windowMissedFrames_;
        if (sample.spinCapped) ++windowCappedFrames_;

        const double longFrameLimitMs = refBaseline_.valid
            ? (std::max)(100.0, 4.0 * refBaseline_.medianPresentMs)
            : 100.0;
        if (sample.presentIntervalMs > longFrameLimitMs)
        {
            ++windowLongFrames_;
        }

        presentSamples_.push_back(sample.presentIntervalMs);
        if (sample.gpuWaitMs >= 0.0) gpuWaitSamples_.push_back(sample.gpuWaitMs);
        if (sample.gpuFrameSpanMs >= 0.0) gpuSpanSamples_.push_back(sample.gpuFrameSpanMs);

        // Immediate safety abort for candidate B if cap/miss/long-frame occurs.
        if (phase_ == PhaseKind::ExploreCandidateB || phase_ == PhaseKind::ConfirmWinnerB)
        {
            if (windowMissedFrames_ > 0 || windowCappedFrames_ > 0 || windowLongFrames_ > 0)
            {
                WindowSummary aborted {};
                aborted.valid = false;
                aborted.aborted = true;
                aborted.candidate = activeCandidate_;
                AdvancePhase(aborted);
                return;
            }
        }

        if (presentSamples_.size() >= windowTargetFrames_)
        {
            WindowSummary summary = FinalizeWindow();
            AdvancePhase(summary);
        }
    }

private:
    static double ComputePercentile(std::vector<double> values, double q)
    {
        if (values.empty()) return 0.0;
        std::sort(values.begin(), values.end());
        if (values.size() == 1) return values.front();
        const double pos = q * static_cast<double>(values.size() - 1);
        const std::size_t idx = static_cast<std::size_t>(pos);
        const double frac = pos - static_cast<double>(idx);
        if (idx + 1 < values.size())
            return values[idx] * (1.0 - frac) + values[idx + 1] * frac;
        return values[idx];
    }

    void RebuildSearchList()
    {
        baselineCandidate_ = NormalizeCandidate(cfg_.fixedPollDelay, cfg_.fixedSlices, { 16, 64 });
        exploreCandidates_.clear();
        for (const auto& c : kWhitelist)
        {
            if (cfg_.fixedPollDelay.has_value() && c.pollDelay != baselineCandidate_.pollDelay) continue;
            if (cfg_.fixedSlices.has_value() && c.slices != baselineCandidate_.slices) continue;
            if (c == baselineCandidate_) continue;
            exploreCandidates_.push_back(c);
        }
    }

    void EnterInactiveOrSuspended(State targetState, SuspendReason reason)
    {
        if (state_ != targetState || reason_ != reason)
        {
            ++epoch_;
        }
        state_ = targetState;
        reason_ = reason;
        phase_ = PhaseKind::None;
        hasValidWorkload_ = false;
        extentUnsettled_ = false;
        pendingExtentFrames_ = 0;
        activeCandidate_ = NormalizeCandidate(cfg_.fixedPollDelay, cfg_.fixedSlices, { 16, 64 });
        baselineCandidate_ = activeCandidate_;
        bestCandidate_ = activeCandidate_;
        ClearWindowBuffers();
    }

    void BeginStabilizing()
    {
        ++epoch_;
        RebuildSearchList();
        state_ = State::Stabilizing;
        reason_ = SuspendReason::None;
        phase_ = PhaseKind::None;
        activeCandidate_ = baselineCandidate_;
        bestCandidate_ = baselineCandidate_;
        refBaseline_ = {};
        preBaselineA_ = {};
        candidateB_ = {};
        postBaselineA_ = {};
        confirmWinnerB_ = {};
        bestSummary_ = {};
        stabilizeFrames_ = 0;
        stabilizeElapsedMs_ = 0.0;
        exploreIndex_ = 0;
        windowRetryCount_ = 0;
        extentUnsettled_ = false;
        pendingExtentFrames_ = 0;
        lockedCooldownFrames_ = 0;
        sceneDriftFrames_ = 0;
        ClearWindowBuffers();
    }

    void StartInitialBaseline()
    {
        state_ = State::Exploring;
        exploreIndex_ = 0;
        windowRetryCount_ = 0;
        BeginWindow(PhaseKind::InitialBaselineA, baselineCandidate_, kMeasureWindowFrames, false);
    }

    void BeginWindow(PhaseKind phase, Candidate cand, uint32_t targetFrames, bool withCooldown)
    {
        const bool switched = (cand != activeCandidate_);
        phase_ = phase;
        activeCandidate_ = cand;
        windowTargetFrames_ = targetFrames;
        cooldownRemaining_ = (withCooldown || switched) ? kSwitchCooldownFrames : 0;
        ClearWindowBuffers();
    }

    void ResetCurrentWindowWithCooldown()
    {
        cooldownRemaining_ = kSwitchCooldownFrames;
        ClearWindowBuffers();
    }

    void ClearWindowBuffers()
    {
        windowTotalFrames_ = 0;
        windowInlineFrames_ = 0;
        windowMissedFrames_ = 0;
        windowCappedFrames_ = 0;
        windowLongFrames_ = 0;
        presentSamples_.clear();
        gpuWaitSamples_.clear();
        gpuSpanSamples_.clear();
    }

    WindowSummary FinalizeWindow() const
    {
        WindowSummary s {};
        s.candidate = activeCandidate_;
        s.totalFrames = windowTotalFrames_;
        s.inlineFrames = windowInlineFrames_;
        s.missedFrames = windowMissedFrames_;
        s.cappedFrames = windowCappedFrames_;
        s.longFrames = windowLongFrames_;
        s.coverage = s.totalFrames > 0
            ? static_cast<double>(s.inlineFrames) / static_cast<double>(s.totalFrames)
            : 0.0;
        if (presentSamples_.empty()) return s;

        s.medianPresentMs = ComputePercentile(presentSamples_, 0.50);
        s.p95PresentMs = ComputePercentile(presentSamples_, 0.95);
        if (!gpuWaitSamples_.empty())
            s.medianGpuWaitMs = ComputePercentile(gpuWaitSamples_, 0.50);
        if (!gpuSpanSamples_.empty())
            s.medianGpuSpanMs = ComputePercentile(gpuSpanSamples_, 0.50);

        s.valid = (s.coverage >= kMinCoverage) &&
                  (s.missedFrames == 0) &&
                  (s.cappedFrames == 0) &&
                  (s.longFrames == 0) &&
                  (s.medianPresentMs > 0.0);
        return s;
    }

    static bool IsDriftAcceptable(const WindowSummary& a1, const WindowSummary& a2)
    {
        if (!a1.valid || !a2.valid || a1.medianPresentMs <= 0.0) return false;
        const double diff = std::abs(a2.medianPresentMs - a1.medianPresentMs) / a1.medianPresentMs;
        return diff <= kMaxBaselineDriftRatio;
    }

    static WindowSummary CombineBaselines(const WindowSummary& a1, const WindowSummary& a2)
    {
        WindowSummary c = a1;
        c.medianPresentMs = 0.5 * (a1.medianPresentMs + a2.medianPresentMs);
        c.p95PresentMs = 0.5 * (a1.p95PresentMs + a2.p95PresentMs);
        if (a1.medianGpuWaitMs >= 0.0 && a2.medianGpuWaitMs >= 0.0)
            c.medianGpuWaitMs = 0.5 * (a1.medianGpuWaitMs + a2.medianGpuWaitMs);
        if (a1.medianGpuSpanMs >= 0.0 && a2.medianGpuSpanMs >= 0.0)
            c.medianGpuSpanMs = 0.5 * (a1.medianGpuSpanMs + a2.medianGpuSpanMs);
        return c;
    }

    static bool IsCandidateBetter(const WindowSummary& cand, const WindowSummary& ref)
    {
        if (!cand.valid || !ref.valid || ref.medianPresentMs <= 0.0) return false;
        // Guardrail: p95 must not regress by more than 3%
        if (cand.p95PresentMs > ref.p95PresentMs * kMaxP95RegressionRatio) return false;

        const double presentDeltaMs = ref.medianPresentMs - cand.medianPresentMs;
        const double presentGainRatio = presentDeltaMs / ref.medianPresentMs;

        // Primary win: present interval improves by >= 1.5% or >= 0.20 ms
        if (presentGainRatio >= kConfirmPresentGainRatio || presentDeltaMs >= kConfirmPresentGainAbsMs)
        {
            return true;
        }

        // Secondary tie-breaker: present interval within +/-0.5% AND same-queue GPU wait improves >= 0.25 ms
        if (std::abs(presentGainRatio) <= kConfirmGpuTiePresentRatio &&
            cand.medianGpuWaitMs >= 0.0 && ref.medianGpuWaitMs >= 0.0)
        {
            const double gpuWaitDeltaMs = ref.medianGpuWaitMs - cand.medianGpuWaitMs;
            if (gpuWaitDeltaMs >= kConfirmGpuWaitGainMs)
            {
                return true;
            }
        }
        return false;
    }

    void AdvancePhase(const WindowSummary& summary)
    {
        switch (phase_)
        {
        case PhaseKind::InitialBaselineA:
        {
            if (!summary.valid)
            {
                if (windowRetryCount_ < kMaxWindowRetry)
                {
                    ++windowRetryCount_;
                    BeginWindow(PhaseKind::InitialBaselineA, baselineCandidate_, kMeasureWindowFrames, true);
                }
                else
                {
                    LockTo(baselineCandidate_, summary);
                }
                return;
            }
            refBaseline_ = summary;
            preBaselineA_ = summary;
            bestCandidate_ = baselineCandidate_;
            bestSummary_ = summary;
            windowRetryCount_ = 0;
            if (exploreCandidates_.empty())
            {
                LockTo(baselineCandidate_, refBaseline_);
                return;
            }
            exploreIndex_ = 0;
            BeginWindow(PhaseKind::ExploreCandidateB, exploreCandidates_[exploreIndex_], kMeasureWindowFrames, true);
            return;
        }

        case PhaseKind::ExplorePreBaselineA:
        {
            if (!summary.valid)
            {
                if (windowRetryCount_ < kMaxWindowRetry)
                {
                    ++windowRetryCount_;
                    BeginWindow(PhaseKind::ExplorePreBaselineA, baselineCandidate_, kMeasureWindowFrames, true);
                }
                else
                {
                    FinishExploration();
                }
                return;
            }
            preBaselineA_ = summary;
            refBaseline_ = summary;
            BeginWindow(PhaseKind::ExploreCandidateB, exploreCandidates_[exploreIndex_], kMeasureWindowFrames, true);
            return;
        }

        case PhaseKind::ExploreCandidateB:
        {
            candidateB_ = summary;
            if (!candidateB_.valid || candidateB_.aborted)
            {
                // Candidate B failed or aborted; return to baseline A and evaluate it as next pre-baseline.
                AdvanceToNextExploreCandidate(/*reusePostAsPre=*/false);
                return;
            }
            BeginWindow(PhaseKind::ExplorePostBaselineA, baselineCandidate_, kMeasureWindowFrames, true);
            return;
        }

        case PhaseKind::ExplorePostBaselineA:
        {
            postBaselineA_ = summary;
            if (!postBaselineA_.valid || !IsDriftAcceptable(preBaselineA_, postBaselineA_))
            {
                if (windowRetryCount_ < kMaxWindowRetry)
                {
                    ++windowRetryCount_;
                    BeginWindow(PhaseKind::ExplorePreBaselineA, baselineCandidate_, kMeasureWindowFrames, true);
                    return;
                }
                // Scene stayed unstable; skip this candidate and re-establish pre-baseline A.
                windowRetryCount_ = 0;
                AdvanceToNextExploreCandidate(/*reusePostAsPre=*/false);
                return;
            }

            const WindowSummary localBaseline = CombineBaselines(preBaselineA_, postBaselineA_);
            refBaseline_ = postBaselineA_;
            if (IsCandidateBetter(candidateB_, localBaseline))
            {
                if (bestCandidate_ == baselineCandidate_ || IsCandidateBetter(candidateB_, bestSummary_))
                {
                    bestCandidate_ = candidateB_.candidate;
                    bestSummary_ = candidateB_;
                }
            }

            windowRetryCount_ = 0;
            preBaselineA_ = postBaselineA_;
            AdvanceToNextExploreCandidate(/*reusePostAsPre=*/true);
            return;
        }

        case PhaseKind::ConfirmWinnerB:
        {
            confirmWinnerB_ = summary;
            if (!confirmWinnerB_.valid || confirmWinnerB_.aborted)
            {
                LockTo(baselineCandidate_, refBaseline_);
                return;
            }
            BeginWindow(PhaseKind::ConfirmBaselineA, baselineCandidate_, kConfirmWindowFrames, true);
            return;
        }

        case PhaseKind::ConfirmBaselineA:
        {
            if (!summary.valid || !IsDriftAcceptable(refBaseline_, summary))
            {
                LockTo(baselineCandidate_, summary.valid ? summary : refBaseline_);
                return;
            }
            if (IsCandidateBetter(confirmWinnerB_, summary))
            {
                LockTo(bestCandidate_, confirmWinnerB_);
            }
            else
            {
                LockTo(baselineCandidate_, summary);
            }
            return;
        }

        default:
            break;
        }
    }

    void AdvanceToNextExploreCandidate(bool reusePostAsPre)
    {
        // One retry belongs to the candidate that failed, not to whoever is next.
        windowRetryCount_ = 0;
        ++exploreIndex_;
        if (exploreIndex_ >= exploreCandidates_.size())
        {
            FinishExploration();
            return;
        }
        if (reusePostAsPre && preBaselineA_.valid)
        {
            BeginWindow(PhaseKind::ExploreCandidateB, exploreCandidates_[exploreIndex_], kMeasureWindowFrames, true);
        }
        else
        {
            BeginWindow(PhaseKind::ExplorePreBaselineA, baselineCandidate_, kMeasureWindowFrames, true);
        }
    }

    void FinishExploration()
    {
        if (bestCandidate_ != baselineCandidate_)
        {
            state_ = State::Confirming;
            windowRetryCount_ = 0;
            BeginWindow(PhaseKind::ConfirmWinnerB, bestCandidate_, kConfirmWindowFrames, true);
        }
        else
        {
            LockTo(baselineCandidate_, refBaseline_);
        }
    }

    void LockTo(Candidate chosen, const WindowSummary& lockedRef)
    {
        state_ = State::Locked;
        phase_ = PhaseKind::None;
        activeCandidate_ = chosen;
        bestCandidate_ = chosen;
        if (lockedRef.valid)
        {
            bestSummary_ = lockedRef;
            lockedRefPresentMs_ = lockedRef.medianPresentMs;
        }
        else if (refBaseline_.valid)
        {
            lockedRefPresentMs_ = refBaseline_.medianPresentMs;
        }
        else
        {
            lockedRefPresentMs_ = 0.0;
        }
        lockedCooldownFrames_ = kReevalCooldownFrames;
        sceneDriftFrames_ = 0;
        ClearWindowBuffers();
    }

    void ObserveLockedSample(const SampleInput& sample)
    {
        // If locked on a non-baseline candidate and safety errors occur, immediately fall back to baseline and re-stabilize.
        if (activeCandidate_ != baselineCandidate_ && (sample.captureMissed || sample.spinCapped))
        {
            BeginStabilizing();
            return;
        }

        if (lockedCooldownFrames_ > 0)
        {
            --lockedCooldownFrames_;
            return;
        }

        if (sample.presentIntervalMs <= 0.0 || lockedRefPresentMs_ <= 0.0)
        {
            return;
        }

        const double shift = std::abs(sample.presentIntervalMs - lockedRefPresentMs_) / lockedRefPresentMs_;
        if (shift > kSceneChangeShiftRatio)
        {
            ++sceneDriftFrames_;
            if (sceneDriftFrames_ >= kSceneChangeHoldFrames)
            {
                BeginStabilizing();
            }
        }
        else
        {
            sceneDriftFrames_ = 0;
        }
    }

    RuntimeConfigInput cfg_ {};
    WorkloadSignature workload_ {};
    bool hasValidWorkload_ = false;
    uint32_t pendingWidth_ = 0;
    uint32_t pendingHeight_ = 0;
    uint32_t pendingExtentFrames_ = 0;
    bool extentUnsettled_ = false;
    State state_ = State::Inactive;
    SuspendReason reason_ = SuspendReason::InlineDisabled;
    PhaseKind phase_ = PhaseKind::None;
    uint64_t epoch_ = 1;

    Candidate baselineCandidate_ { 16, 64 };
    Candidate activeCandidate_ { 16, 64 };
    Candidate bestCandidate_ { 16, 64 };
    std::vector<Candidate> exploreCandidates_ {};
    uint32_t exploreIndex_ = 0;

    uint32_t stabilizeFrames_ = 0;
    double stabilizeElapsedMs_ = 0.0;

    uint32_t cooldownRemaining_ = 0;
    uint32_t windowTargetFrames_ = kMeasureWindowFrames;
    uint32_t windowRetryCount_ = 0;

    uint32_t windowTotalFrames_ = 0;
    uint32_t windowInlineFrames_ = 0;
    uint32_t windowMissedFrames_ = 0;
    uint32_t windowCappedFrames_ = 0;
    uint32_t windowLongFrames_ = 0;
    std::vector<double> presentSamples_ {};
    std::vector<double> gpuWaitSamples_ {};
    std::vector<double> gpuSpanSamples_ {};

    WindowSummary refBaseline_ {};
    WindowSummary preBaselineA_ {};
    WindowSummary candidateB_ {};
    WindowSummary postBaselineA_ {};
    WindowSummary confirmWinnerB_ {};
    WindowSummary bestSummary_ {};

    double lockedRefPresentMs_ = 0.0;
    uint32_t lockedCooldownFrames_ = 0;
    uint32_t sceneDriftFrames_ = 0;
};

} // namespace InlineAutoSpin
} // namespace DlssNr::Backend
