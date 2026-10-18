#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrCoverage.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

int main()
{
    using namespace DlssNr::Coverage;
    Counters c;
    // The reported game can miss proxies without consuming an NR frame id.
    // These misses must remain visible even below the old 1000-warning threshold.
    for (int i = 0; i < 5; ++i) { c.Begin(); c.Finish(false, Outcome::ProxyMissing); }
    for (int i = 0; i < 100; ++i) { c.Begin(); c.Finish(false, Outcome::BuildWait); }
    const auto warmup = c.Read();
    c.prepared.store(true);
    for (int i = 0; i < 900; ++i) {
        c.Begin(); c.Finish(c.prepared.load(), Outcome::ProxyMissing);
        c.Begin(); c.Finish(c.prepared.load(), Outcome::Recorded);
    }
    const auto steady = Delta(c.Read(), warmup);
    assert(steady.calls[0][unsigned(Outcome::ProxyMissing)] == 0);
    assert(steady.calls[1][unsigned(Outcome::ProxyMissing)] == 900);
    assert(steady.calls[1][unsigned(Outcome::Recorded)] == 900);
    assert(c.offered.load() == 1905);
    // Retained/replayed recordings can report more execution callbacks than calls.
    // Callback counters are independently thread-safe and do not rewrite coverage.
    const auto before = c.Read();
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) workers.emplace_back([&] {
        for (int i = 0; i < 1000; ++i) {
            ++c.executions;
            ++c.enqueues;
            if (i % 2) ++c.inlineApplied; else ++c.inlineMissed;
        }
    });
    for (auto& w : workers) w.join();
    const auto d = Delta(c.Read(), before);
    assert(d.executions == 4000 && d.enqueues == 4000);
    assert(d.inlineApplied == 2000 && d.inlineMissed == 2000);
    assert(d.calls[1][unsigned(Outcome::Recorded)] == 0);
    assert(c.offered.load() == 1905);
    c.prepared.store(false); // A new session's build waits are startup again.
    c.Begin(); c.Finish(c.prepared.load(), Outcome::BuildWait);
    assert(Delta(c.Read(), before).calls[0][unsigned(Outcome::BuildWait)] == 1);
    c.Begin(); c.Finish(false, Outcome::Rejected, "predication");
    c.Begin(); c.Finish(false, Outcome::InputRejected, "enhanced_layout_unknown");
    const auto rejected = c.Read();
    assert(rejected.rejections[0][0] == 1 && rejected.rejections[0][1] == 1);
    assert(rejected.rejections[1][0] == 0);
    std::cout << "NR coverage: PASS\n";
}
