#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/MotionExtent.h"
#include <cassert>
#include <cstdio>

int main()
{
    using DlssNr::DisplayMotionExtent;
    // Stellar Blade pre-SR: MVLowRes clear, motion allocation is the display
    // size, the picture is the render subrect. Extent stays zero.
    const auto stellar = DisplayMotionExtent(true, true, 2558, 1439, 3840, 2160);
    assert(stellar.width == 0 && stellar.height == 0);
    // The same flag after SR still names the output colour, not the allocation.
    const auto after = DisplayMotionExtent(false, true, 3840, 2160, 4096, 2176);
    assert(after.width == 3840 && after.height == 2160);
    // A missing output side falls back to that side of the allocation only.
    const auto partial = DisplayMotionExtent(false, true, 0, 2160, 4096, 2176);
    assert(partial.width == 4096 && partial.height == 2160);
    // MVLowRes, before or after SR, keeps render-resolution vectors.
    const auto lowBefore = DisplayMotionExtent(true, false, 2558, 1439, 3840, 2160);
    const auto lowAfter = DisplayMotionExtent(false, false, 3840, 2160, 3840, 2160);
    assert(lowBefore.width == 0 && lowBefore.height == 0);
    assert(lowAfter.width == 0 && lowAfter.height == 0);
    std::puts("motion extent: PASS (pre-SR render subrect, post-SR display, low-res)");
}
