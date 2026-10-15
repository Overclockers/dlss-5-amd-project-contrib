#pragma once

namespace DlssNr
{
// Extent stored on the NR frame. Zero means the vectors match the render
// picture, and the game's MV scale is already in those pixels. A non-zero
// pair is a display-sized region, used only after SR.
struct MotionExtent
{
    unsigned width = 0;
    unsigned height = 0;
};

// Pre-SR always stays zero. A display-sized motion allocation can still hold
// the render picture in its corner (Stellar Blade: 3840x2160 around
// 2558x1439) while MVLowRes is clear. Promoting that to OutWidth, or to the
// whole allocation, makes the runtime divide the scale by the output size
// and resample the unused margin.
// After SR, with MVLowRes clear, the extent is the output colour size. Each
// side falls back to the motion allocation only when that side was not resolved.
inline MotionExtent DisplayMotionExtent(bool beforeUpscale, bool fullResolutionMotion,
                                       unsigned outputWidth, unsigned outputHeight,
                                       unsigned motionAllocationWidth, unsigned motionAllocationHeight)
{
    if (beforeUpscale || !fullResolutionMotion)
        return {};
    MotionExtent extent;
    extent.width = outputWidth ? outputWidth : motionAllocationWidth;
    extent.height = outputHeight ? outputHeight : motionAllocationHeight;
    return extent;
}
} // namespace DlssNr
