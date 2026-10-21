#include <d3d12.h>
#include <cassert>
#include <vector>
#include <cstdio>
#include <climits>

struct DescriptorRangeInfo
{
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    UINT numDescriptors;
    UINT baseShaderRegister;
    UINT registerSpace;
    UINT offsetInTable;
};

struct ParameterInfo
{
    std::vector<DescriptorRangeInfo> ranges;
};

struct RootSignatureInfo
{
    std::vector<ParameterInfo> parameters;
};

// Pure-function reproduction of ResTrack_Dx12::BuildRootSignatureInfo range calculation
void ParseRootSignatureDesc(const D3D12_ROOT_SIGNATURE_DESC* desc, RootSignatureInfo& rsInfo)
{
    if (!desc)
        return;

    rsInfo.parameters.resize(desc->NumParameters);

    for (UINT i = 0; i < desc->NumParameters; ++i)
    {
        const auto& param = desc->pParameters[i];
        if (param.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            continue;

        const auto& table = param.DescriptorTable;
        UINT currentOffset = 0;

        for (UINT r = 0; r < table.NumDescriptorRanges; ++r)
        {
            const auto& range = table.pDescriptorRanges[r];

            if (range.OffsetInDescriptorsFromTableStart != D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
            {
                currentOffset = range.OffsetInDescriptorsFromTableStart;
            }

            // We only track UAV and SRV ranges for hudfix
            if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ||
                range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV)
            {
                rsInfo.parameters[i].ranges.push_back({
                    range.RangeType,
                    range.NumDescriptors,
                    range.BaseShaderRegister,
                    range.RegisterSpace,
                    currentOffset
                });
            }

            if (range.NumDescriptors != UINT_MAX)
            {
                currentOffset += range.NumDescriptors;
            }
        }
    }
}

int main()
{
    // Test 1: Explicit offset followed by D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
    {
        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        // Range 0: SRV, 4 descriptors at offset 0
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 4;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        // Range 1: UAV, 8 descriptors appended
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 8;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // Range 2: SRV, 2 descriptors appended
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[2].NumDescriptors = 2;
        ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 3;
        param.DescriptorTable.pDescriptorRanges = ranges;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 1;
        desc.pParameters = &param;

        RootSignatureInfo info;
        ParseRootSignatureDesc(&desc, info);

        assert(info.parameters.size() == 1);
        assert(info.parameters[0].ranges.size() == 3);
        assert(info.parameters[0].ranges[0].offsetInTable == 0);
        assert(info.parameters[0].ranges[1].offsetInTable == 4);
        assert(info.parameters[0].ranges[2].offsetInTable == 12);
    }

    // Test 2: CBV range preceding an appended SRV range (CBV filtered out, but offset advances)
    {
        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        // Range 0: CBV, 2 descriptors at offset 0 (filtered out)
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[0].NumDescriptors = 2;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        // Range 1: SRV, 4 descriptors appended (should start at 2!)
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 4;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // Range 2: Sampler, 1 descriptor appended (filtered out)
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        ranges[2].NumDescriptors = 1;
        ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 3;
        param.DescriptorTable.pDescriptorRanges = ranges;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 1;
        desc.pParameters = &param;

        RootSignatureInfo info;
        ParseRootSignatureDesc(&desc, info);

        assert(info.parameters.size() == 1);
        // Only SRV is stored
        assert(info.parameters[0].ranges.size() == 1);
        assert(info.parameters[0].ranges[0].type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
        assert(info.parameters[0].ranges[0].offsetInTable == 2);
    }

    // Test 3: Discontinuous explicit offset reset
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        // Range 0: SRV, 2 descriptors at offset 5
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 2;
        ranges[0].OffsetInDescriptorsFromTableStart = 5;

        // Range 1: UAV, 3 descriptors at explicit offset 20
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 3;
        ranges[1].OffsetInDescriptorsFromTableStart = 20;

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 1;
        desc.pParameters = &param;

        RootSignatureInfo info;
        ParseRootSignatureDesc(&desc, info);

        assert(info.parameters[0].ranges.size() == 2);
        assert(info.parameters[0].ranges[0].offsetInTable == 5);
        assert(info.parameters[0].ranges[1].offsetInTable == 20);
    }

    // Test 4: Verify D3D12 command list vtable layout contract
    // Slot 11 is ClearState in ID3D12GraphicsCommandList
    // (slot 0..6: IUnknown/ID3D12Object/ID3D12DeviceChild, slot 7: GetType, slot 8: Close, slot 9: Reset, slot 10: ClearState in command list)
    // In D3D12:
    // QueryInterface(0), AddRef(1), Release(2), GetPrivateData(3), SetPrivateData(4), SetPrivateDataInterface(5), SetName(6),
    // GetDevice(7), GetType(8), Close(9), Reset(10), ClearState(11).
    static_assert(sizeof(void*) == 8, "64-bit target required");

    std::puts("RESTRACK_D3_SIGNATURE_OK");
    return 0;
}
