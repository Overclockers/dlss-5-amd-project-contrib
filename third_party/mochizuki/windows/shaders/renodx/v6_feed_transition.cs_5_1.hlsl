// Bridge feed changes from the divisor actually used by the previous frame. The v1
// and v2 estimators keep their own state; ordinary game exposure is not rate limited.
#include "vk_binding.hlsli"
#include "feed_continuity.hlsli"
VK_BINDING(4) RWTexture2D<float4> Scale : register(u0);
VK_BINDING(5) RWTexture2D<float4> Continuity : register(u1);
PUSH_CONSTANTS(TransitionConstants) {
    uint Feed;
    uint Reset;
    float Attack;
    float Release;
    float Seconds;
};
[numthreads(1, 1, 1)]
void main() {
    const float4 old = Continuity[uint2(0, 0)];
    float4 scale = Scale[uint2(0, 0)];
    const uint source = Feed == 3u ? 1u : Feed;
    const bool valid = old.r > 0.0 && old.r < 16777216.0;
    float value = scale.r;
    bool tracking = valid && (old.b > 0.5 || uint(old.g) != source);
    if (Reset == 0u && valid) {
        if (Feed == 3u) value = old.r;
        else if (tracking) {
            value = FeedStep(old.r, max(scale.r, 5.96046448e-8), Attack, Release, clamp(Seconds, 0.0, 0.1));
            tracking = abs(log2(max(scale.r, 5.96046448e-8) / value)) > 0.0001;
        }
    } else tracking = false;
    scale.r = value;
    Scale[uint2(0, 0)] = scale;
    Continuity[uint2(0, 0)] = float4(value, float(source), tracking ? 1.0 : 0.0, 0.0);
}
