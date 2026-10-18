#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfModelOptions.h"
#include "third_party/lmxxf/src/native_network_geometry.h"

static void check(bool ok) { if(!ok)throw std::runtime_error("model selection contract"); }
int main() {
    for(auto key:{"DLSS5_NETWORK_MODEL","DLSS5_MULTI_PASS","DLSS5_TEMPORAL_MODE","DLSS5_SKIP_BLOCKS"})_putenv_s(key,"");
    _putenv_s("DLSS5_SKIP_BLOCKS","none");
    auto resolve=[](bool assets=true){return LmxxfResolveModel("modules","weights",assets);};
    check(!resolve().useSmall);
    check(!resolve().options.integration.allow_vit_hotkey&&!resolve().options.integration.allow_input_poll);
    check(!resolve().options.integration.allow_model_prewarm);
    dlss5_mode::Request(0); // Addon hot state must not override product selection.
    check(resolve().options.integration.model_mode==1);
    _putenv_s("DLSS5_MULTI_PASS","0.5");check(resolve().useSmall);
    _putenv_s("DLSS5_NETWORK_MODEL","0");check(!resolve().requestedSmall);
    _putenv_s("DLSS5_NETWORK_MODEL","1");_putenv_s("DLSS5_MULTI_PASS","3");
    auto result=resolve();check(result.useSmall&&result.options.integration.model_mode==0);
    check(!resolve(false).useSmall&&resolve(false).options.integration.model_mode==3);
    _putenv_s("DLSS5_TEMPORAL_MODE","2");check(!resolve().useSmall);
    _putenv_s("DLSS5_TEMPORAL_MODE","3");check(!resolve().useSmall);
    _putenv_s("DLSS5_TEMPORAL_MODE","1");check(resolve().useSmall);
    _putenv_s("DLSS5_SKIP_BLOCKS","5");check(!resolve().useSmall);
    _putenv_s("DLSS5_SKIP_BLOCKS","none");_putenv_s("DLSS5_NETWORK_MODEL","0");
    check(resolve().options.integration.model_mode==3);
    for(auto h:{720u,900u,1080u}) {
        unsigned w=h==720?1280:h==900?1600:1920;
        auto g=NativeNetworkGeometry::Small(w,h,true);
        check(g.processing_width==(h==720?1344:w));
        check(g.processing_height==(h==720?768:h==900?1024:1152));
    }
    std::puts("Small model selection, fallback, retained Original passes and geometry: PASS");
}
