#pragma once
#include <atomic>

// Initialized by Config before game recordings, independently of backend creation.
inline std::atomic<bool> g_allowEnhancedBarriers { false };
inline void SetAllowEnhancedBarriers(bool on) { g_allowEnhancedBarriers.store(on, std::memory_order_release); }
inline bool AllowEnhancedBarriers() { return g_allowEnhancedBarriers.load(std::memory_order_acquire); }
