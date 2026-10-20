#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNetworkIdentity.h"
static void check(bool ok) { if(!ok) throw std::runtime_error("DRS retention contract"); }
int main()
{
    using namespace LmxxfRuntime;
    const auto tier=NativeNetworkGeometry::FromHeight(1080);
    check(SameGeometry(tier,tier));
    auto different=tier;
    different.vit_w=32; check(!SameGeometry(tier,different));
    different=tier; different.small_tier=true; check(!SameGeometry(tier,different));
    different=tier; different.small_model=true; check(!SameGeometry(tier,different));
    const auto freeA=NativeNetworkGeometry::Free(1280,720);
    const auto freeB=NativeNetworkGeometry::Free(1280,721);
    check(freeA.processing_width==freeB.processing_width && freeA.processing_height==freeB.processing_height);
    check(!SameGeometry(freeA,freeB));
    const auto smallA=NativeNetworkGeometry::Small(1280,720);
    const auto smallB=NativeNetworkGeometry::Small(1280,721);
    check(!SameGeometry(smallA,smallB));
    check(!SameGeometry(smallA,NativeNetworkGeometry::Small(1280,720,true)));
    auto a=LmxxfProductionOptions(1920,1152,"modules","assets");
    auto b=a; check(SameConstruction(a,b));
    b.integration.model_mode=2; check(!SameConstruction(a,b));
    b=a; b.mh_byte_stream=!a.mh_byte_stream; check(!SameConstruction(a,b));
    b=a; b.vit_qkv_frag=!a.vit_qkv_frag; check(!SameConstruction(a,b));
    b=a; b.integration.allow_model_prewarm=!a.integration.allow_model_prewarm; check(!SameConstruction(a,b));
    b=a; b.integration.multi_pass_skip.insert(12); check(!SameConstruction(a,b));
    b=a; b.assets="other"; check(!SameConstruction(a,b));
    for (bool leases:{false,true}) for(bool geometry:{false,true})
        for(bool options:{false,true}) for(bool mapping:{false,true}) for(bool adaptive:{false,true})
            check(CanRetainNetwork(leases,geometry,options,mapping,adaptive)==
                  (leases&&geometry&&options&&(!mapping||!adaptive)));
    std::puts("Exact geometry, construction and adaptive exclusions: PASS");
}
