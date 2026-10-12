#pragma once
#include <algorithm>
#include <cmath>

namespace MenuWindowLayout
{
struct Point { float x, y; };
struct Layout { Point pos, size, minimum, maximum; bool valid; };
inline Layout Calculate(Point origin, Point viewport, float scale, Point preferred,
                        Point position, int anchor, bool center)
{
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        !std::isfinite(viewport.x) || !std::isfinite(viewport.y) || viewport.x <= 0 || viewport.y <= 0)
        return {};
    if (!std::isfinite(scale) || scale <= 0) scale = 1;
    scale = std::clamp(scale, 0.5f, 4.0f);
    const float margin = std::min({12 * scale, viewport.x * .05f, viewport.y * .05f});
    const Point lo {origin.x + margin, origin.y + margin};
    const Point available {viewport.x - 2 * margin, viewport.y - 2 * margin};
    const Point minimum {std::min(480 * scale, available.x), std::min(240 * scale, available.y)};
    auto dimension = [scale](float pref, float fallback, float min, float max) {
        const float value = std::isfinite(pref) && pref > 0 ? pref * scale : fallback;
        return std::clamp(value, min, max);
    };
    // Unsaved default: a narrow column on the right (about 32% x 80% of the
    // display, top at 12%, right edge at 95%), at least 560 logical px wide so
    // 1080p is not cramped. A saved WindowWidth/Height keeps the old behaviour.
    const bool saved = (std::isfinite(preferred.x) && preferred.x > 0) || (std::isfinite(preferred.y) && preferred.y > 0);
    const Point size {dimension(preferred.x, std::max(560 * scale, viewport.x * .32f), minimum.x, available.x),
                      dimension(preferred.y, viewport.y * .80f, minimum.y, available.y)};
    Point pos = position;
    if (center && !saved)
        pos = {origin.x + viewport.x * .95f - size.x, origin.y + viewport.y * .12f};
    else if (center || !std::isfinite(pos.x) || !std::isfinite(pos.y))
        pos = {lo.x + (available.x - size.x) / 2, lo.y + (available.y - size.y) / 2};
    if (anchor >= 1 && anchor <= 4)
        pos = {lo.x + ((anchor == 2 || anchor == 4) ? available.x - size.x : 0),
               lo.y + (anchor >= 3 ? available.y - size.y : 0)};
    pos.x = std::clamp(pos.x, lo.x, lo.x + available.x - size.x);
    pos.y = std::clamp(pos.y, lo.y, lo.y + available.y - size.y);
    return {pos, size, minimum, available, true};
}
}
