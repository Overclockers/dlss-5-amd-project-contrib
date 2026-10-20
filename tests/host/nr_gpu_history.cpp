#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrGpuHistory.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <type_traits>

using namespace DlssNr;
static GpuHistoryObservation Point(uint64_t samples, double ms = 2, uint64_t tick = 1000)
{
    return {GpuHistoryMetric::Network, 1, 0, samples, tick, ms, true, true};
}

int main()
{
    static_assert(std::is_trivially_copyable_v<GpuHistory>);
    static_assert(sizeof(GpuHistory) < 2048); // bounded inline storage, no owning allocations
    GpuHistory history;
    history.Update({}, 1000);
    assert(!history.HasMeasurement());
    auto point = Point(1, 0);
    history.Update(point, 1000);
    assert(history.Size() == 1 && history.Last() == 0); // measured zero is not missing
    for (int i = 0; i < 1000; ++i) history.Update(point, 1000);
    assert(history.Size() == 1);
    point.tick = 1100;
    point.milliseconds = 99;
    history.Update(point, 1100); // a poll timestamp or changed summary is not progress
    assert(history.Size() == 1 && history.Last() == 0);
    point = Point(9, 4, 1100);
    history.Update(point, 1100);
    assert(history.Size() == 2 && history.At(0) == 0 && history.At(1) == 4);
    // A count jump contributes only the actually observed last execution.
    point = Point(10, 5, 1100);
    history.Update(point, 1100);
    assert(history.Size() == 3); // multiple completions in one clock tick are distinct
    history.Update(point, 3100);
    assert(history.Size() == 3); // inclusive freshness boundary
    history.Update(point, 3101);
    assert(!history.HasMeasurement());
    history.Update(point, 1100);
    assert(!history.HasMeasurement()); // a stale sample never gets replayed
    point = Point(11, 6, 3200);
    history.Update(point, 3200);
    assert(history.Size() == 1 && history.Last() == 6);
    point.enabled = false;
    history.Update(point, 3200);
    assert(!history.HasMeasurement());
    point.enabled = true;
    history.Update(point, 3200);
    assert(!history.HasMeasurement());
    point.samples++;
    history.Update(point, 3200);
    assert(history.Size() == 1);
    point.route++;
    history.Update(point, 3200);
    assert(!history.HasMeasurement()); // reject cached previous-route execution
    point.samples++;
    history.Update(point, 3200);
    assert(history.Size() == 1);
    point.runtime++;
    point.samples = 50; // restart count can exceed old count between UI polls
    history.Update(point, 3200);
    assert(history.Size() == 1);
    point.metric = GpuHistoryMetric::MochizukiVulkanCore;
    history.Update(point, 3200);
    assert(history.Size() == 1 && history.Metric() == GpuHistoryMetric::MochizukiVulkanCore);
    point.samples = 1;
    history.Update(point, 3200);
    assert(history.Size() == 1); // counter rollback partitions even without an epoch change
    point.samples = 2;
    point.tick = 3000;
    history.Update(point, 3200);
    assert(history.Size() == 1); // completion clock rollback partitions
    for (double invalid : {-1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN(), (std::numeric_limits<double>::max)()})
    {
        ++point.samples;
        point.milliseconds = invalid;
        history.Update(point, 3200);
        assert(!history.HasMeasurement());
        point.milliseconds = 1;
        history.Update(point, 3200);
        assert(!history.HasMeasurement());
    }
    point = Point(100, 1, 4000);
    history.Update(point, 3999);
    assert(!history.HasMeasurement());
    history.Update(point, 4000);
    assert(!history.HasMeasurement()); // future-dated observation is consumed, not deferred
    point.runtime = 0;
    ++point.samples;
    history.Update(point, 4000);
    assert(!history.HasMeasurement());
    history.Update({}, 4000); // Daniel / unavailable source is backend-neutral N/A
    assert(!history.HasMeasurement() && history.Metric() == GpuHistoryMetric::Unavailable);

    GpuHistory ring;
    for (uint64_t i = 1; i <= GpuHistory::Capacity + 17; ++i)
        ring.Update(Point(i, static_cast<double>(i), 1000), 1000);
    assert(ring.Size() == GpuHistory::Capacity && ring.At(0) == 18);
    for (size_t i = 0; i < ring.Size(); ++i) assert(ring.At(i) == i + 18);

    NrTimingSnapshot timing {};
    timing.struct_size = sizeof timing;
    timing.version = NR_TIMING_VERSION;
    timing.enabled = 1;
    timing.stages[NR_CPU_PREPARE] = {9, 25, 25, 25, 1000, 8, 9};
    auto network = NetworkHistoryObservation(timing, true, 1, 0);
    assert(!network.measured); // never CPU wall time
    timing.stages[NR_GPU_NETWORK] = {1, 0, 42, 99, 1000, 123, 321};
    network = NetworkHistoryObservation(timing, true, 1, 0);
    assert(network.measured && network.samples == 1 && network.milliseconds == 0);
    GpuHistory adapter;
    adapter.Update(network, 1000);
    timing.stages[NR_GPU_NETWORK].frame_id++;
    timing.stages[NR_GPU_NETWORK].execution_id++;
    timing.stages[NR_GPU_NETWORK].last_tick_ms++;
    adapter.Update(NetworkHistoryObservation(timing, true, 1, 0), 1001);
    assert(adapter.Size() == 1); // only accepted monotonic samples counts deduplicate
    timing.enabled = 0;
    assert(!NetworkHistoryObservation(timing, true, 1, 0).measured);
    timing.enabled = 1;
    timing.version++;
    assert(!NetworkHistoryObservation(timing, true, 1, 0).measured);
    timing.version = NR_TIMING_VERSION;
    timing.struct_size--;
    assert(!NetworkHistoryObservation(timing, true, 1, 0).measured);

    MochizukiNrInfo mochi {};
    mochi.struct_size = sizeof mochi;
    mochi.frames = 9999;
    mochi.gpu_ms_median = 8;
    auto core = MochizukiHistoryObservation(mochi, true, 2, 0);
    assert(!core.measured && core.samples == 0); // frames/median cannot synthesize measurements
    mochi.gpu_samples = 1;
    mochi.gpu_tick = 1000;
    mochi.gpu_ms_last = 0;
    core = MochizukiHistoryObservation(mochi, true, 2, 0);
    assert(core.measured && core.milliseconds == 0 && core.tick == 1000);
    adapter.Update(core, 1000);
    assert(adapter.Size() == 1 && adapter.Last() == 0);
    mochi.frames++;
    mochi.gpu_ms_median = 20;
    adapter.Update(MochizukiHistoryObservation(mochi, true, 2, 0), 1000);
    assert(adapter.Size() == 1 && adapter.Last() == 0);
    mochi.failed = 1;
    adapter.Update(MochizukiHistoryObservation(mochi, true, 2, 0), 1000);
    assert(!adapter.HasMeasurement());
    mochi.failed = 0;
    mochi.struct_size--;
    assert(!MochizukiHistoryObservation(mochi, true, 2, 0).measured);
    std::cout << "nr GPU history: PASS\n";
}
