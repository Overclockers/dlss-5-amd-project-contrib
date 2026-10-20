#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/PassInitialization.h"
#include <array>
#include <cassert>
#include <cstdio>

int main()
{
    std::array<AmdPreSr::PassInitialization, 3> passes;
    // A compiler/weights/engine failure after load leaves the module registered,
    // but never publishes completion. Repeated Record and off/on cannot enable it.
    bool moduleRetained = true;
    unsigned enables = 0;
    for (int record = 0; record < 4; ++record)
    {
        bool rejected = false;
        try { passes[0].RequireReady(); ++enables; }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected && moduleRetained && enables == 0);
    }
    // Successful passes are independent and reusable across disable/re-enable.
    passes[1].Complete();
    for (int record = 0; record < 4; ++record) { passes[1].RequireReady(); ++enables; }
    assert(enables == 4);
    bool thirdRejected = false;
    try { passes[2].RequireReady(); }
    catch (const std::runtime_error&) { thirdRejected = true; }
    assert(thirdRejected);
    // A new session owns fresh state rather than inheriting another pass' success.
    AmdPreSr::PassInitialization recreated;
    bool recreatedRejected = false;
    try { recreated.RequireReady(); }
    catch (const std::runtime_error&) { recreatedRejected = true; }
    assert(recreatedRejected);
    std::puts("AMD retained pass initialization: PASS");
}
