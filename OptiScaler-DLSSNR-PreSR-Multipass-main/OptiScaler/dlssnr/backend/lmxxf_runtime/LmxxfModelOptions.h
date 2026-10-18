#pragma once
#include "LmxxfProductionOptions.h"


// Product model selection is independent of Original's retained 1..3 pass setting.
// Build a new bridge/recording chain on changes; never hot-switch an in-flight network.
struct LmxxfModelOptions {
    hip_reference::Options options;
    bool requestedSmall=false, useSmall=false;
    const char* reason="original";
};
inline bool LmxxfSmallRequested() {
    const char* model=std::getenv(CfgKey::EnvAlias(CfgKey::LmxxfNetworkModel));
    const char* pass=std::getenv(CfgKey::MultiPass);
    return model?std::strcmp(model,"1")==0:pass&&std::strcmp(pass,"0.5")==0;
}
inline LmxxfModelOptions LmxxfResolveModel(const std::string& modules, const std::string& assets, bool smallAssetsReady) {
    LmxxfModelOptions result;
    auto& o=result.options;
    o=LmxxfProductionOptions(1920,1152,modules,assets);
    const char* pass=std::getenv(CfgKey::MultiPass);
    result.requestedSmall=LmxxfSmallRequested();
    unsigned passes=pass&&std::strcmp(pass,"2")==0?2u:pass&&std::strcmp(pass,"3")==0?3u:1u;
    if(result.requestedSmall) {
        if(!smallAssetsReady)result.reason="missing-small-assets";
        else if(!hip_reference::WaveOwnedCompatible(o)||!hip_reference::C512M32Compatible(o)||
                !hip_reference::VitProjN64Compatible(o)||o.graph||o.experimental_temporal||o.temporal_feature_tap||!o.skip_blocks.empty())result.reason="incompatible-options";
        else if(const char* mode=std::getenv(CfgKey::TemporalMode);mode&&
                (std::strcmp(mode,"2")==0||std::strcmp(mode,"3")==0))result.reason="low-frequency-requires-original";
        else {result.useSmall=true;result.reason="small-ready";}
    }
    o.small_geometry=result.useSmall;
    o.integration.allow_small_model=result.useSmall;
    o.integration.model_mode=result.useSmall?0:int(passes);
    return result;
}
