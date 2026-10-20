#pragma once
#include "LmxxfProductionOptions.h"
#include "native_network_geometry.h"
#include <tuple>

namespace LmxxfRuntime {
inline bool SameGeometry(const NativeNetworkGeometry& a, const NativeNetworkGeometry& b)
{
    return std::tie(a.valid_width,a.valid_height,a.processing_width,a.processing_height,
                    a.vit_w,a.vit_h,a.small_model,a.small_tier) ==
           std::tie(b.valid_width,b.valid_height,b.processing_width,b.processing_height,
                    b.vit_w,b.vit_h,b.small_model,b.small_tier);
}
// All Options fields, including diagnostic/layout switches, belong to construction.
// select_module is the fixed product callback in LmxxfProductionOptions, not caller-supplied.
inline bool SameConstruction(const hip_reference::Options& a, const hip_reference::Options& b)
{
#define LMXXF_OPTIONS(o) std::tie(o.small_geometry,o.submit_pulse,o.experimental_temporal,o.temporal_valid_height, \
 o.temporal_feature_tap,o.swin_run,o.vit_stream,o.wave_owned,o.c512_m32,o.vit_proj_n64,o.pdl, \
 o.mh_ffn_frag256,o.decoder_byte,o.skip_blocks,o.width,o.height,o.seed,o.post_shift,o.assets,o.modules, \
 o.dump_dir,o.dump_only,o.ffn_qkv_max_c,o.runtime,o.device,o.tiled_ffn_min_c,o.vit_weight_mask, \
 o.mh_window_fused,o.mh_proj_diag_fb,o.sparse_weights,o.sparse_filter,o.sparse_full,o.vit_qkv_fused, \
 o.ffn_qkv,o.grouped_mh_contract,o.direct_prefix_input,o.prefix_fused,o.mh_input_mapped,o.mh_project_crop, \
 o.vit_qkv_blocked,o.split_project_blocked,o.split_mix_blocked,o.split_ffn_fused,o.fused_ffn_project, \
 o.post_merge_fold,o.pre_main8,o.raw_chain,o.elide_identity_shift,o.fast_vit,o.wmma,o.pooled,o.profile, \
 o.wall_profile,o.wave,o.tiled,o.fast_c32,o.fused_c32,o.fast_mh,o.fast_deep,o.fast_prefix,o.mh_wave, \
 o.fused_ffn,o.fused_mh,o.packed_weights,o.packed_c32,o.fp8_normalized,o.fp8_ffn,o.fp8_av,o.fp8_deep, \
 o.fp8_middle,o.half_c32,o.crop_c32,o.fused_qkv_norm,o.vit_pack_input,o.vit_contract_blocked,o.vit_blocked, \
 o.vit_split_k,o.mapped_c32,o.fused_mh_ffn,o.tiled_mh_ffn,o.graph,o.vit_attn_fused,o.vit_qkv_fp8, \
 o.mh_byte_stream,o.vit_expand_m4,o.vit_expand_frag,o.vit_byte_stream,o.vit_half_stream,o.vit_qkv_n4, \
 o.ffn_qkv_batched_norm,o.pool_project_fused,o.qkv_norm_wave_c512,o.vit_expand_m2,o.vit_input_tiled, \
 o.vit_ffn_fused,o.split_mix_fused,o.split_mix_h16w,o.pool_project_h16w,o.decoder_h16w,o.mh_attn_w16, \
 o.c512_qkv_frag,o.c512_proj_frag,o.c512_proj_tiles,o.mh_feature_byte,o.mh_proj_diag,o.post_head_fused, \
 o.c32_finish_fused,o.down_crop_fused,o.pool32_h16w,o.tiled_ffn_small,o.pool_project_group,o.prefix_inline, \
 o.vit_proj_frag,o.vit_qkv_frag,o.vit_contract_frag,o.mh_ffn_frag,o.dup_count,o.dup_prefix, \
 o.integration.allow_vit_hotkey,o.integration.allow_input_poll,o.integration.allow_small_model, \
 o.integration.allow_model_prewarm,o.integration.model_mode,o.integration.override_multi_pass_skip, \
 o.integration.multi_pass_skip)
    return LMXXF_OPTIONS(a) == LMXXF_OPTIONS(b);
#undef LMXXF_OPTIONS
}
inline bool CanRetainNetwork(bool leases, bool sameGeometry, bool sameConstruction,
                             bool mappingChanged, bool adaptiveEnabled)
{
    return leases && sameGeometry && sameConstruction && !(mappingChanged && adaptiveEnabled);
}
} // namespace LmxxfRuntime
