#pragma once
#include "NrPerformance.h"
#include "backend/mochizuki_runtime/MochizukiNrControls.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace DlssNr
{
enum class GpuHistoryMetric { Unavailable, Network, MochizukiVulkanCore };

struct GpuHistoryObservation
{
    GpuHistoryMetric metric = GpuHistoryMetric::Unavailable;
    uint64_t runtime = 0, route = 0, samples = 0, tick = 0;
    double milliseconds = 0;
    bool enabled = false, measured = false;
};

inline GpuHistoryObservation NetworkHistoryObservation(const NrTimingSnapshot& timing, bool enabled,
                                                       uint64_t runtime, uint64_t route)
{
    GpuHistoryObservation out {GpuHistoryMetric::Network, runtime, route};
    out.enabled = enabled;
    if (timing.version != NR_TIMING_VERSION || timing.struct_size != sizeof timing || !timing.enabled)
        return out;
    const auto& value = timing.stages[NR_GPU_NETWORK];
    out.samples = value.samples;
    out.tick = value.last_tick_ms;
    out.milliseconds = value.last_ms;
    out.measured = value.samples != 0;
    return out;
}

inline GpuHistoryObservation MochizukiHistoryObservation(const MochizukiNrInfo& timing, bool enabled,
                                                         uint64_t runtime, uint64_t route)
{
    GpuHistoryObservation out {GpuHistoryMetric::MochizukiVulkanCore, runtime, route};
    out.enabled = enabled;
    if (timing.struct_size != sizeof timing || timing.failed) return out;
    out.samples = timing.gpu_samples;
    out.tick = timing.gpu_tick;
    out.milliseconds = timing.gpu_ms_last;
    out.measured = timing.gpu_samples != 0;
    return out;
}

// One point per newly observed completed measurement, not per UI poll. Cached snapshots
// can skip executions: no missing points are invented or replayed from summary statistics.
class GpuHistory
{
public:
    static constexpr size_t Capacity = 360;
    static constexpr uint64_t StaleMilliseconds = 2000;

    void Update(const GpuHistoryObservation& value, uint64_t now)
    {
        const bool sourceChanged = value.metric != metric_ || value.runtime != runtime_;
        const bool routeChanged = !sourceChanged && value.route != route_;
        if (sourceChanged)
        {
            Clear();
            samples_ = tick_ = 0;
        }
        else if (routeChanged)
        {
            Clear();
            // A cached observation at a route switch may still belong to the old route.
            samples_ = value.samples;
            tick_ = value.tick;
        }
        metric_ = value.metric;
        runtime_ = value.runtime;
        route_ = value.route;
        const bool valid = value.enabled && value.runtime && value.measured && value.samples &&
            value.metric != GpuHistoryMetric::Unavailable && std::isfinite(value.milliseconds) &&
            value.milliseconds >= 0 && value.milliseconds <= (std::numeric_limits<float>::max)() &&
            now >= value.tick && now - value.tick <= StaleMilliseconds;
        if (!valid)
        {
            Clear();
            // Preserve the watermark through stale/missing/disabled polls. The same
            // execution cannot come back as a new point on re-enable or clock rollback.
            if (value.samples) { samples_ = value.samples; tick_ = value.tick; }
            return;
        }
        if (value.samples == samples_) return;
        if (value.samples < samples_ || value.tick < tick_) Clear();
        samples_ = value.samples;
        tick_ = value.tick;
        values_[cursor_] = static_cast<float>(value.milliseconds);
        cursor_ = (cursor_ + 1) % Capacity;
        if (size_ < Capacity) ++size_;
    }

    size_t Size() const { return size_; }
    bool HasMeasurement() const { return size_ != 0; }
    GpuHistoryMetric Metric() const { return metric_; }
    float At(size_t index) const { return values_[(cursor_ + Capacity - size_ + index) % Capacity]; }
    float Last() const { return size_ ? At(size_ - 1) : 0; }

private:
    void Clear() { size_ = cursor_ = 0; }
    std::array<float, Capacity> values_ {};
    size_t size_ = 0, cursor_ = 0;
    GpuHistoryMetric metric_ = GpuHistoryMetric::Unavailable;
    uint64_t runtime_ = 0, route_ = 0, samples_ = 0, tick_ = 0;
};
}
