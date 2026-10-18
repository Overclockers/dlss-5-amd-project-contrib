#pragma once
#include <vector>
#include <filesystem>
#include <fstream>
#include "native_lab_paths.h"
// Fourth post projection in the HIP feature order. The unique column mapping
// exactly matches all three existing RGB rows; no model coefficients are tuned.
// Float32 row SHA256: 8c8a821c6c258eb07368ad2c36769e1e9debcb53f8786fbcaa1c82029d223952
inline std::vector<float> LmxxfNativePostWeights(){ return {
    1.17492675781250000e-02f, 5.94615936279296875e-04f, -4.51278686523437500e-03f, 1.08871459960937500e-02f,
    -1.43127441406250000e-02f, 1.50871276855468750e-03f, 1.96075439453125000e-02f, -7.74002075195312500e-03f,
    -1.31702423095703125e-03f, 6.36291503906250000e-03f, -1.38092041015625000e-02f, 4.69970703125000000e-03f,
    -4.36305999755859375e-04f, -1.00021362304687500e-02f, 2.73590087890625000e-02f, -4.31823730468750000e-03f,
    1.18026733398437500e-02f, -2.90489196777343750e-03f, -4.73976135253906250e-04f, -1.77307128906250000e-02f,
    -9.16719436645507812e-05f, 4.24194335937500000e-03f, -3.45039367675781250e-03f, -8.17871093750000000e-03f,
    3.26232910156250000e-02f, -1.60217285156250000e-02f, 4.85595703125000000e-01f, -7.54547119140625000e-03f,
    -1.30615234375000000e-02f, -1.80912017822265625e-03f, -4.81262207031250000e-02f, 1.04751586914062500e-02f
}; }

// The Small row is generated from the same verified model as its RGB weights.
inline std::vector<float> LmxxfNativePostWeights(const std::wstring& assets,bool useSmall) {
    if(!useSmall)return LmxxfNativePostWeights();
    std::ifstream f(std::filesystem::path(assets)/L"small"/L"post70-history-head.f16",std::ios::binary);
    std::vector<uint16_t> half(32);
    if(!f.read(reinterpret_cast<char*>(half.data()),64)||f.peek()!=EOF)
        throw std::runtime_error("Small model history row missing; reinstall Small assets");
    std::vector<float> row;row.reserve(32);
    for(auto h:half)row.push_back(NativeHalfToFloat(h));
    return row;
}
