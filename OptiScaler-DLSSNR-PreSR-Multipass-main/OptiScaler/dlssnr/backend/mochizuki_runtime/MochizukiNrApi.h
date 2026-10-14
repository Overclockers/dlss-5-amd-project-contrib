#pragma once
#include "../lmxxf_runtime/LmxxfNrApi.h"
#include "MochizukiNrControls.h"

// Separate frame contract; the shared v2 table supplies recording ownership and
// submission operations. It never reinterprets lmxxf's frame or the older fork ABI.
#define MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL (1u << 8)
// This frame records on the inline side queue. Absent means command-list split.
// Create-time LMXXF_NR_CREATE_FLAG_INLINE_QUEUE only permits the side queue.
#define MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION (1u << 9)
// Diagnostic: GPU timestamp pairs around the core's linear-input encode and transfer pass
// (MochizukiNrInfo::gpu_ms_encode_last / gpu_ms_transfer_last). Absent: none are written.
#define MOCHIZUKI_NR_FRAME_FLAG_STAGE_TIMING (1u << 10)
// NrInlineComputeConvert (default off): an inline B10G11R11 (R11G11B10_FLOAT) colour frame converts its
// input on the compute family (copy + runtime_unpack_f16) instead of a graphics blit, so the whole
// chain can stay on compute. Only honoured where the write-back needs no blit; may rebuild the network.
#define MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_CONVERT (1u << 11)
typedef struct MochizukiNrFrameInfo
{
    uint32_t struct_size;
    uint64_t frame_id;
    void* color;
    uint32_t color_width, color_height, color_state, flags;
    float transfer_strength, color_strength, model_scale;
    uint32_t passes;
    void* motion;
    uint32_t motion_state, motion_width, motion_height;
    float motion_scale_x, motion_scale_y;
    uint32_t reset;
} MochizukiNrFrameInfo;

typedef int32_t (*PFN_MochizukiNrPrepareFrame)(void*, const MochizukiNrFrameInfo*, LmxxfNrJob*);
typedef int32_t (*PFN_MochizukiNrGetApi)(uint32_t, LmxxfNrApi*);

#ifdef __cplusplus
extern "C" {
#endif
MOCHIZUKI_NR_EXPORT int32_t MochizukiNrGetApi(uint32_t version, LmxxfNrApi* out);
MOCHIZUKI_NR_EXPORT int32_t MochizukiNrPrepareFrame(void*, const MochizukiNrFrameInfo*, LmxxfNrJob*);
#ifdef __cplusplus
}
#endif
