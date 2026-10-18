"""Run the production feed decisions and transition shader body on CPU, without Vulkan."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / 'third_party/mochizuki/windows/src/core'
SHADERS = ROOT / 'third_party/mochizuki/windows/shaders/renodx'
OUT = ROOT / 'exports/mochizuki-tests/renodx-feed'


def main():
    source = (CORE / 'nr_renodx.cpp').read_text(encoding='utf-8')
    decisions = source[source.index('enum Feed : uint32_t'):source.index('// ---- Preprocess on this path')]
    runtime = (ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp').read_text(encoding='utf-8')
    settle_start = runtime.index('static const char* const kBuilding', runtime.index('bool EnsureNetwork('))
    settle = runtime[settle_start:runtime.index('        bool finished = false;', settle_start)]
    policy_start = runtime.index('            const bool extentShrinking =')
    policy = runtime[policy_start:runtime.index('            if (!s->EnsureNetwork', policy_start)]
    extent = r'''
using ULONGLONG = unsigned long long;
ULONGLONG GetTickCount64() { return ULONGLONG(test_now / 1'000'000); }
constexpr ULONGLONG kExactExtentSettleMs = 300;
namespace nr { template<class... T> void logf(const char*, T...) {} }
struct ExtentKey { unsigned width=1708, height=960; bool operator==(const ExtentKey&) const = default; };
struct ExtentGeometry {
    unsigned long long colourWidth=1508;
    unsigned colourHeight=848, colourFormat=10, motionWidth=1506, motionHeight=847, motionFormat=34;
    bool bucketed=true;
};
struct ExtentSession {
    bool runtime=true, extentCandidatePending=false;
    ExtentKey net, extentCandidate;
    ULONGLONG extentCandidateSince=0;
    unsigned extentDeferrals=0;
    bool Ensure(const ExtentKey& key, const ExtentGeometry& g, std::string& why, bool settleExtent) {
''' + settle + r'''
        return true;
    }
    bool Prepare(const ExtentKey& key, const ExtentGeometry& g) {
        auto* s=this; std::string why;
''' + policy + r'''
        return Ensure(key,g,why,settleExtent);
    }
};
'''
    shader = (SHADERS / 'v6_feed_transition.cs_5_1.hlsl').read_text(encoding='utf-8')
    shader = re.sub(r'^#include.*$', '', shader, flags=re.M)
    shader = re.sub(r'VK_BINDING\(\d+\) RWTexture2D<float4> (\w+) : register\(u\d+\);', r'Texture \1;', shader)
    shader = shader.replace('PUSH_CONSTANTS(TransitionConstants) {', '').replace('};', '')
    shader = shader.replace('[numthreads(1, 1, 1)]', '').replace('void main()', 'void transition_gpu()')
    # HLSL unsuffixed decimals are float; C++ otherwise promotes them to double.
    shader = re.sub(r'(?<![\w.])(\d+\.\d*(?:e[-+]?\d+)?)(?![\w.])', r'\1f', shader)
    prefix = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
using std::clamp; using std::exp2; using std::log2; using std::abs; using std::max;
using uint = uint32_t;
int64_t test_now = 1'000'000'000;
int64_t now_ns() { return test_now; }
struct Settings {
    uint32_t feed_mode = 0, norm_governor = 2;
    bool norm_cut_aware = false;
    float norm_attack_stops = 4.f, norm_release_stops = .5f, norm_slew_stops = 1.f;
};
namespace codec {
enum class Encoding { Sdr, Linear };
struct SourceUnits { bool absolute = false; Encoding encoding = Encoding::Linear; };
}
struct uint2 { uint2(uint, uint) {} };
struct float4 {
    float r=0, g=0, b=0, a=0;
    float4() = default;
    float4(float x, float y, float z, float w): r(x),g(y),b(z),a(w) {}
};
struct Texture { float4 value; float4& operator[](uint2) { return value; } };
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
'''
    tests = r'''
int main() {
    ExtentSession extent; ExtentGeometry geometry; ExtentKey small{1508,848};
    Require(!extent.Prepare(small,geometry), "bucket shrink rebuilt on its first frame");
    test_now += 299'000'000;
    Require(!extent.Prepare(small,geometry), "bucket shrink ignored settle window");
    Require(extent.Prepare(extent.net,geometry), "restored old size did not resume");
    Require(!extent.extentCandidatePending, "restoration kept stale settle candidate");
    Require(!extent.Prepare(small,geometry), "new shrink reused old timer");
    test_now += 300'000'000;
    Require(extent.Prepare(small,geometry), "stable shrink never admitted");
    extent.runtime=false;
    Require(extent.Prepare(small,geometry), "startup was needlessly delayed");
    Settings s; codec::SourceUnits u; NormStream n;
    auto feed = select_feed(s, 1, u, true, false, n, test_now, true);
    Require(feed == kFeedTexture, "trusted exposure not selected");
    plan_frame_scale(s, n, false, feed, false);
    test_now += 10'000'000;
    Require(select_feed(s, 1, u, false, false, n, test_now, false) == kFeedHold,
            "Auto: temporary missing sample must hold despite unknown flags");
    Require(select_feed(s, 1, u, false, true, n, test_now, true) == kFeedV1,
            "known AutoExposure must not hold a game texture");
    NormStream empty;
    Require(select_feed(s, 1, u, false, false, empty, test_now, false) == kFeedV1,
            "cannot hold before first valid sample");
    test_now += 600'000'000;
    Require(select_feed(s, 1, u, false, false, n, test_now, false) == kFeedV1,
            "Auto: hold must expire by elapsed time");
    Require(select_feed(s, 1, u, true, false, n, test_now, true) == kFeedTexture,
            "fresh exposure must recover");
    s.feed_mode = 2;
    test_now += 10'000'000;
    Require(select_feed(s, 1, u, false, false, n, test_now, false) == kFeedHold, "forced: hold");
    test_now += 600'000'000;
    Require(select_feed(s, 1, u, false, false, n, test_now, false) == kFeedFixed, "forced: timeout");
    for (uint mode=0; mode<3; ++mode) for (bool cut : {false, true}) {
        s.norm_governor=mode; s.norm_cut_aware=cut;
        NormStream stream;
        plan_frame_scale(s, stream, false, kFeedTexture, false);
        auto plan=plan_frame_scale(s, stream, false, kFeedV1, false);
        Require(plan.governor[1] == bits(0.f) && !plan.v2_snap, "feed change forced a snap");
    }
    // Execute the actual HLSL body using CPU texture intrinsics, including both directions,
    // hold during a transition, timeout/fixed feed and explicit reset.
    Attack=4.f; Release=.5f; Seconds=1.f/60.f; Reset=0;
    Continuity.value=float4(4.f, 1.f, 0.f, 0.f);
    Scale.value=float4(1.f, 0.f, 0.f, 0.f); Feed=0;
    transition_gpu();
    Require(Scale.value.r < 4.f && Scale.value.r > 3.9f, "v2->v1 jumped instead of slewing");
    float held=Scale.value.r;
    Feed=3; Scale.value.r=2.f; transition_gpu();
    Require(Scale.value.r == held, "Hold must preserve the actually applied divisor");
    Feed=1; Scale.value.r=8.f; transition_gpu();
    Require(Scale.value.r > held && Scale.value.r < 4.3f, "recovery jumped to reading");
    for (int i=0;i<200;++i) { Scale.value.r=8.f; transition_gpu(); }
    Require(Continuity.value.b == 0.f, "transition never completes");
    Scale.value.r=7.f; transition_gpu();
    Require(Scale.value.r == 7.f, "ordinary game exposure was needlessly filtered");
    Reset=1; Scale.value.r=2.f; transition_gpu();
    Require(Scale.value.r == 2.f, "explicit reset must re-prime");
    std::puts("renodx feed: PASS (extent settle/restore, Auto/forced Hold, timeout, recovery, governors, production shader transitions)");
}
'''
    OUT.mkdir(parents=True, exist_ok=True)
    cpp = OUT / 'feed.cpp'
    cpp.write_text(prefix + decisions + extent + (SHADERS / 'feed_continuity.hlsli').read_text() + shader + tests,
                   encoding='utf-8')
    exe = OUT / 'feed.exe'
    subprocess.run(['cl', '/nologo', '/std:c++20', '/EHsc', '/W4', '/utf-8', str(cpp),
                    '/Fo' + str(OUT / 'feed.obj'), '/Fe' + str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
