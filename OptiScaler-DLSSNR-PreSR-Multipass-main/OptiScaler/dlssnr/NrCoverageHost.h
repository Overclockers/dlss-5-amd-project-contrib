#pragma once
#include "NrCoverage.h"
#include "NrFrameDiagHost.h"
#include "backend/ProxyDiagnostics.h"
#include <memory>
#include <string>

namespace DlssNr::Coverage
{
// Record-thread state is protected by the existing bridge/recording mutex.
// Submission callbacks retain Counters independently of the backend lifetime.
class Tracker {
    const char* name;
    bool bridge;
    uint64_t last = 0;
    Snapshot previous {};
    std::array<std::array<uint64_t, kOutcomes + kReasons>, 2> detailAt {};
    bool detailed = false;
public:
    std::shared_ptr<Counters> counters = std::make_shared<Counters>();
    explicit Tracker(const char* backend) : name(backend), bridge(std::strcmp(backend, "bridge") == 0) {}
    const char* Phase(bool ready) const noexcept {
        return bridge ? (ready ? "after_first_replacement" : "before_first_replacement")
                      : (ready ? "after_first_prepare" : "before_first_prepare");
    }
    void Flush(bool final = false) noexcept {
        try {
            const auto* c = Config::Instance();
            if (!c || (!c->LogToFile.value_or_default() && !c->NrFlickerDiag.value_or_default())) return;
            const auto tick = GetTickCount64();
            if (!final && last && tick - last < 5000) return;
            const auto total = counters->Read(), delta = Delta(total, previous);
            if (!counters->offered.load(std::memory_order_relaxed) && !total.executions) return;
            for (unsigned p = 0; p < 2; ++p) {
                uint64_t calls = 0, recorded = delta.calls[p][unsigned(Outcome::Recorded)];
                std::string reasons;
                for (unsigned i = 0; i < kOutcomes; ++i) {
                    calls += delta.calls[p][i];
                    if (delta.calls[p][i]) {
                        if (!reasons.empty()) reasons += ',';
                        reasons += kNames[i]; reasons += '='; reasons += std::to_string(delta.calls[p][i]);
                    }
                }
                std::string causes;
                for (unsigned i = 0; i < kReasons; ++i) if (delta.rejections[p][i]) {
                    if (!causes.empty()) causes += ',';
                    causes += kReasonNames[i]; causes += '='; causes += std::to_string(delta.rejections[p][i]);
                }
                if (calls || final)
                    LOG_INFO("NR coverage backend={} phase={} final={} tick={} window_ms={} calls={} recorded={} mixed={} reasons=[{}] reject_causes=[{}] cumulative_offered={}",
                        name, Phase(p != 0), final, tick, last ? tick - last : 0,
                        calls, recorded, recorded != 0 && recorded != calls, reasons, causes, counters->offered.load(std::memory_order_relaxed));
            }
            LOG_INFO("NR coverage execution backend={} final={} tick={} callbacks={} failed={} enqueues={} enqueue_failed={} split_accepted={} inline_applied={} inline_missed={} cumulative_callbacks={} units=executions_not_presents",
                name, final, tick, delta.executions, delta.executionFailed, delta.enqueues, delta.enqueueFailed, delta.splitAccepted,
                delta.inlineApplied, delta.inlineMissed, total.executions);
            previous = total; last = tick;
        } catch (...) {} // Logging failure must not change rendering.
    }
    void Finish(uint64_t call, bool ready, Outcome outcome, const char* reason,
                ID3D12GraphicsCommandList* list, uint64_t listId, uint64_t generation) noexcept {
        counters->Finish(ready, outcome, reason);
        try {
            const auto* c = Config::Instance();
            const bool on = c && c->NrFlickerDiag.value_or_default();
            if (on != detailed) {
                detailed = on; detailAt = {};
                const auto preference = [](const auto& option) {
                    return !option.has_value() ? "auto" : (option.value() ? "on" : "off");
                };
                if (on) LOG_INFO("NR flicker diagnostics backend={} enabled; coverage units=calls, readiness=first successful PrepareFrame (bridge: first replacement); inline={} enhanced_preference={} early_exe_preference={} early_any_preference={} pre_sr={}; collect OptiScaler*.log, mochizuki_nr.log*, NrFrameDiag.log*, OptiScaler.ini",
                    name, c->NrInline.value_or_default(), preference(c->LmxxfAllowEnhancedBarriers),
                    preference(c->LmxxfEarlyExeWrap), preference(c->LmxxfEarlyAnyModuleWrap), c->DlssNrRunBeforeSr.value_or_default());
            }
            const auto tick = GetTickCount64();
            const auto detailKey = outcome == Outcome::Rejected || outcome == Outcome::InputRejected
                ? kOutcomes + RejectReason(reason) : unsigned(outcome);
            auto& at = detailAt[ready ? 1 : 0][detailKey];
            // One sample per outcome per five seconds, independent of cumulative miss count.
            if (on && (!at || tick - at >= 5000)) {
                at = tick;
                char origin[MAX_PATH * 2] {};
                if (outcome == Outcome::ProxyMissing) Backend::ProxyDiagnostics::DescribeList(list, origin, sizeof origin);
                LOG_INFO("NR coverage detail backend={} call={} tick={} present={} phase={} outcome={} reason={} list={:p} listId={} generation={} {}",
                    name, call, tick, FrameDiag::PresentSerialAtom().load(std::memory_order_relaxed),
                    Phase(ready), kNames[unsigned(outcome)], reason,
                    static_cast<void*>(list), listId, generation, origin);
            }
        } catch (...) {}
        Flush();
    }
};
class Attempt {
    Tracker& tracker;
    uint64_t call;
    Outcome outcome = Outcome::RecordFailed;
    char reason[160] = "recording_failed";
    ID3D12GraphicsCommandList* list;
    uint64_t listId = 0, generation = 0;
public:
    Attempt(Tracker& t, ID3D12GraphicsCommandList* cmd) noexcept
        : tracker(t), call(t.counters->Begin()), list(cmd) {}
    void Set(Outcome value, const char* why) noexcept {
        outcome = value; std::snprintf(reason, sizeof reason, "%s", why ? why : "unknown");
    }
    void Identity(uint64_t id, uint64_t gen) noexcept { listId = id; generation = gen; }
    void Prepared() noexcept { tracker.counters->prepared.store(true, std::memory_order_relaxed); }
    ~Attempt() { tracker.Finish(call, tracker.counters->prepared.load(std::memory_order_relaxed), outcome, reason, list, listId, generation); }
};
}
