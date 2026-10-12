#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/MenuWindowLayout.h"
#include <cassert>
#include <limits>
#include <iostream>

int main()
{
    using namespace MenuWindowLayout;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    assert(!Calculate({0, 0}, {0, 0}, 1, {}, {}, 0, true).valid);
    assert(!Calculate({0, 0}, {nan, 720}, 1, {}, {}, 0, true).valid);
    // Include small/offset viewports, oversized saved preferences, every anchor and UI scale.
    for (float width : {100.f, 640.f, 1280.f, 1920.f, 3840.f})
    for (float height : {80.f, 360.f, 720.f, 1080.f, 2160.f})
    for (float scale : {.5f, 1.f, 2.f, nan})
    for (int anchor = 0; anchor <= 5; ++anchor)
    for (Point pref : {Point{0, 0}, Point{99999, 99999}, Point{nan, -1}, Point{600, 400}})
    {
        const auto l = Calculate({100, 200}, {width, height}, scale, pref, {-10000, 10000}, anchor, false);
        assert(l.valid && std::isfinite(l.pos.x) && std::isfinite(l.pos.y));
        assert(l.minimum.x <= l.maximum.x && l.minimum.y <= l.maximum.y);
        assert(l.pos.x >= 100 && l.pos.y >= 200);
        assert(l.pos.x + l.size.x <= 100 + width + .01f);
        assert(l.pos.y + l.size.y <= 200 + height + .01f);
    }
    const auto tl = Calculate({100, 200}, {1920, 1080}, 1, {600, 400}, {}, 1, false);
    const auto br = Calculate({100, 200}, {1920, 1080}, 1, {600, 400}, {}, 4, false);
    assert(tl.pos.x == 112 && tl.pos.y == 212);
    assert(br.pos.x + br.size.x == 2008 && br.pos.y + br.size.y == 1268);
    const Point preferred {1000, 800};
    const auto small = Calculate({}, {640, 360}, 1, preferred, {}, 0, true);
    const auto restored = Calculate({}, {1920, 1080}, 1, preferred, {}, 0, true);
    assert(small.size.x < preferred.x && restored.size.x == preferred.x && restored.size.y == preferred.y);
    // Unsaved default: right column, clamped on screen at 1080p/1440p/2160p.
    for (Point vp : {Point{1920, 1080}, Point{2560, 1440}, Point{3840, 2160}, Point{1280, 720}})
    {
        const float sc = vp.y >= 2160 ? 2.f : vp.y >= 1440 ? 1.3f : vp.y >= 1080 ? 1.f : .6f;
        const auto d = Calculate({}, vp, sc, {}, {}, 0, true);
        assert(d.size.x >= std::min(560 * sc, d.maximum.x) && d.size.x <= d.maximum.x);
        assert(d.pos.x + d.size.x <= vp.x + .01f && d.pos.y + d.size.y <= vp.y + .01f);
        assert(d.pos.x > vp.x * .4f);
        std::cout << vp.x << "x" << vp.y << " scale " << sc << ": " << d.size.x << "x" << d.size.y
                  << " at " << d.pos.x << "," << d.pos.y << "\n";
    }
    std::cout << "menu window layout: PASS\n";
}
