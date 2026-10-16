#pragma once
#include "TemporalSupport.h"
#include "../../../../../third_party/lmxxf/src/native_shader_cache.h"
#include "../../../../../third_party/lmxxf/src/native_low_frequency_shader.h"
#include <wrl/client.h>
#include <vector>

// Native model pre/post history. The host serializes GPU users; each
// recording pins its own guides/descriptor heap. Control is updated at execution.
// The two reprojected buffers differ: depth selects pre motion, post uses centre.
namespace LmxxfNativeTemporal
{
using LmxxfTemporal::Check;
using LmxxfTemporal::Transition;
using LmxxfTemporal::MotionFormat;
using LmxxfTemporal::DepthFormat;
inline constexpr char kShader[] = R"hlsl(
// Original model temporal contract: pre uses nearest-depth motion, post uses
// centre motion. Both use the normalized five-tap Catmull-Rom reconstruction.
Texture2D<float2> Motion : register(t0);
Texture2D<float> Depth : register(t1);
StructuredBuffer<float4> Raw : register(t2);
StructuredBuffer<float4> PostWarp : register(t3);
StructuredBuffer<float4> PreviousModel : register(t4);
StructuredBuffer<float4> PreWarp : register(t5);
StructuredBuffer<float4> PreviousRaw : register(t6);
RWStructuredBuffer<float4> PreOut : register(u0);
RWStructuredBuffer<float4> PostOut : register(u1);
RWStructuredBuffer<float4> ModelOut : register(u2);
RWStructuredBuffer<float> Network : register(u3);
RWStructuredBuffer<float4> RawOut : register(u4);
cbuffer Params : register(b0) {
    uint width, height, processingHeight, useHistory;
    uint viewX, viewY, viewWidth, viewHeight;
    uint renderWidth, renderHeight, motionWidth, motionHeight;
    float scaleX, scaleY, jitterX, jitterY;
    float historyStrength; uint logitOffset, depthInverted, hasDepth;
    uint4 reserved;
};
uint2 Mirror(uint i) {
    uint2 p=uint2(i%width,i/width);
    if(p.y>=height)p.y=2*height-p.y-2;
    return p;
}
bool InView(uint2 p) {
    return all(p>=uint2(viewX,viewY)) && all(p<uint2(viewX+viewWidth,viewY+viewHeight));
}
float2 ViewUV(uint2 p) { return (float2(p)+.5-float2(viewX,viewY))/float2(viewWidth,viewHeight); }
float GetDepth(float2 uv) {
    uint2 p=min(uint2(saturate(uv)*float2(renderWidth,renderHeight)),uint2(renderWidth-1,renderHeight-1));
    return Depth.Load(int3(p,0));
}
float2 GetMotion(float2 uv) {
    uint2 p=min(uint2(saturate(uv)*float2(motionWidth,motionHeight)),uint2(motionWidth-1,motionHeight-1));
    return Motion.Load(int3(p,0))*float2(scaleX,scaleY);
}
float4 LinearHistory(float2 centre) {
    // Manual bilinear fetch from float4 buffer, using texel-centre coordinates.
    float2 p=clamp(centre-.5,float2(viewX,viewY),float2(viewX+viewWidth-1,viewY+viewHeight-1));
    uint2 lo=uint2(floor(p)),hi=min(lo+1,uint2(viewX+viewWidth-1,viewY+viewHeight-1));
    float2 f=p-lo;
    float4 a=PreviousModel[lo.y*width+lo.x],b=PreviousModel[lo.y*width+hi.x];
    float4 c=PreviousModel[hi.y*width+lo.x],d=PreviousModel[hi.y*width+hi.x];
    float4 value=lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
    // Alpha is validity, not blend weight. Ignore zero-weight bilinear taps.
    bool valid=(a.w>0 || (1-f.x)*(1-f.y)==0) && (b.w>0 || f.x*(1-f.y)==0) &&
               (c.w>0 || (1-f.x)*f.y==0) && (d.w>0 || f.x*f.y==0);
    return float4(value.rgb,valid?1:0);
}
float4 History5(float2 pos) {
    float2 lo=float2(viewX,viewY)+.5,hi=lo+float2(viewWidth,viewHeight)-1;
    float2 centre=floor(pos-.5)+.5,t=saturate(pos-centre),t2=t*t,t3=t2*t;
    float2 w0=(t+t3)*-.5+t2,w1=1.5*t3-2.5*t2+1,w3=.5*(t3-t2),w2=1-w0-w1-w3,wc=w1+w2;
    float2 p0=clamp(centre-1,lo,hi),pm=clamp(centre+w2/wc,lo,hi),p2=clamp(centre+2,lo,hi);
    float4 left=LinearHistory(float2(p0.x,pm.y)),up=LinearHistory(float2(pm.x,p0.y));
    float4 mid=LinearHistory(pm),down=LinearHistory(float2(pm.x,p2.y)),right=LinearHistory(float2(p2.x,pm.y));
    float kl=w0.x*wc.y,ku=w0.y*wc.x,km=wc.x*wc.y,kd=w3.y*wc.x,kr=w3.x*wc.y;
    float3 result=(kl*left.rgb+ku*up.rgb+km*mid.rgb+kd*down.rgb+kr*right.rgb)/(kl+ku+km+kd+kr);
    bool valid=(left.w>0||kl==0)&&(up.w>0||ku==0)&&(mid.w>0||km==0)&&(down.w>0||kd==0)&&(right.w>0||kr==0);
    return float4(result,valid&&all(isfinite(result))?1:0);
}
bool HistoryCompatible(float2 centre,float3 raw,float depth) {
    // Preserve the host's existing raw/depth disocclusion guards. This is an
    // integration guard around the native blend, not a new smoothing strength.
    float2 p=clamp(centre-.5,float2(viewX,viewY),float2(viewX+viewWidth-1,viewY+viewHeight-1));
    uint2 lo=uint2(floor(p)),hi=min(lo+1,uint2(viewX+viewWidth-1,viewY+viewHeight-1));float2 f=p-lo;
    float4 a=PreviousRaw[lo.y*width+lo.x],b=PreviousRaw[lo.y*width+hi.x];
    float4 c=PreviousRaw[hi.y*width+lo.x],d=PreviousRaw[hi.y*width+hi.x];
    float3 old=lerp(lerp(a.rgb,b.rgb,f.x),lerp(c.rgb,d.rgb,f.x),f.y);
    if(!all(isfinite(old))||any(abs(old-raw)>.08))return false;
    if(hasDepth) {
        float4 prior=float4(a.w,b.w,c.w,d.w),tolerance=1e-5+.05*max(abs(prior),abs(depth));
        if(!all(isfinite(prior))||any(prior<0)||any(abs(prior-depth)>tolerance))return false;
    }
    return true;
}
float4 ReprojectAt(float2 uv,float2 motion,float3 raw,float depth) {
    float2 q=(uv+motion+float2(jitterX,jitterY))*float2(viewWidth,viewHeight)+float2(viewX,viewY);
    if(!all(isfinite(q)))return 0;
    if(!HistoryCompatible(q,raw,depth))return 0;
    return History5(q);
}
void ReprojectPixel(uint i,out float4 preValue,out float4 postValue) {
    float3 raw=Raw[i].rgb;preValue=float4(raw,0);postValue=float4(raw,0);
    uint2 p=Mirror(i);if(!useHistory||!InView(p)||!all(isfinite(raw)))return;
    float2 uv=ViewUV(p),muv=uv;
    if(hasDepth) {
        float best=GetDepth(uv);float2 step=1.0/float2(renderWidth,renderHeight);
        if(!isfinite(best))return;
        [unroll]for(int dy=-1;dy<=1;dy+=2)[unroll]for(int dx=-1;dx<=1;dx+=2) {
            float2 at=uv+float2(dx,dy)*step;float d=GetDepth(at);
            if(isfinite(d)&&(depthInverted?d>best:d<best)){best=d;muv=at;}
        }
    }
    float depth=hasDepth?GetDepth(uv):0;
    depth=depthInverted?depth:1-depth;
    float4 pre=ReprojectAt(uv,GetMotion(muv),raw,depth),post=ReprojectAt(uv,GetMotion(uv),raw,depth);
    if(pre.w>0)preValue=pre;
    if(post.w>0)postValue=post;
}
[numthreads(64,1,1)]
void Reproject(uint3 id:SV_DispatchThreadID) {
    if(id.x>=width||id.y>=processingHeight)return;
    uint i=id.y*width+id.x;float4 pre,post;ReprojectPixel(i,pre,post);
    PreOut[i]=pre;PostOut[i]=post;
}
[numthreads(64,1,1)]
void Finish(uint3 id:SV_DispatchThreadID) {
    if(id.x>=width||id.y>=processingHeight)return;
    uint i=id.y*width+id.x;
    uint2 pixel=Mirror(i);float depth=hasDepth?GetDepth(ViewUV(pixel)):0;
    depth=depthInverted?depth:1-depth;
    RawOut[i]=float4(Raw[i].rgb,InView(pixel)?depth:-1);
    float3 value=float3(Network[i*3],Network[i*3+1],Network[i*3+2]);
    // Keep the bridge's zero-output failure contract: failed inference must not
    // resurrect an old frame. The host also resets the global history latch.
    bool good=all(isfinite(value))&&all(value>=-.05)&&all(value<=1.5)&&max(max(abs(value.x),abs(value.y)),abs(value.z))>1e-7;
    float4 old=PostWarp[i];
    if(good&&useHistory&&old.w>0) {
        float logit=Network[logitOffset+i*2];
        if(isfinite(logit)) {
            float weight=saturate(historyStrength)*.73974609375/(1+exp(-logit));
            value=lerp(value,old.rgb,weight);
            Network[i*3]=value.x;Network[i*3+1]=value.y;Network[i*3+2]=value.z;
        }else good=false;
    }
    ModelOut[i]=float4(value,good&&InView(Mirror(i))?1:0);

}

)hlsl";
struct Parameters
{
    UINT width{}, height{}, processingHeight{}, useHistory{};
    UINT viewX{}, viewY{}, viewWidth{}, viewHeight{};
    UINT renderWidth{}, renderHeight{}, motionWidth{}, motionHeight{};
    float scaleX{}, scaleY{}, jitterX{}, jitterY{};
    float historyStrength{1}; UINT logitOffset{}, depthInverted{}, hasDepth{1};
    UINT reserved[4]{};
};
static_assert(sizeof(Parameters)==96);
class History
{
    ID3D12Resource *postWarp{}, *previousModel{}, *preWarp{}, *previousRaw{};
    ID3D12RootSignature *root{};
    ID3D12PipelineState *reproject{}, *finish{};
    UINT width{}, height{}, processingHeight{};
    D3D12_RESOURCE_STATES preState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    static ID3D12Resource *Buffer(ID3D12Device *device, UINT64 bytes)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes; desc.Height=1;
        desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource *r=nullptr;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r)),"buffer");
        return r;
    }
    void Bind(ID3D12GraphicsCommandList *cmd, ID3D12Resource *raw, ID3D12Resource *output, const Parameters &, ID3D12DescriptorHeap* heap, D3D12_GPU_VIRTUAL_ADDRESS control)
    {
        cmd->SetDescriptorHeaps(1,&heap);
        cmd->SetComputeRootSignature(root);
        cmd->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        ID3D12Resource *srvs[]={raw,postWarp,previousModel,preWarp};
        for(UINT i=0;i<4;++i) cmd->SetComputeRootShaderResourceView(1+i,srvs[i]->GetGPUVirtualAddress());
        ID3D12Resource *uavs[]={preWarp,postWarp,previousModel,output};
        for(UINT i=0;i<4;++i) cmd->SetComputeRootUnorderedAccessView(5+i,uavs[i]->GetGPUVirtualAddress());
        cmd->SetComputeRootConstantBufferView(9,control);
        cmd->SetComputeRootShaderResourceView(10,previousRaw->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(11,previousRaw->GetGPUVirtualAddress());
    }
public:
    History()=default;
    History(const History&)=delete;
    ~History()
    {
        for(auto *r:{postWarp,previousModel,preWarp,previousRaw}) if(r) r->Release();
        if(root) root->Release();
        if(reproject) reproject->Release();
        if(finish) finish->Release();
    }
    bool Matches(UINT w,UINT h,UINT ph) const { return width==w && height==h && processingHeight==ph; }
    void Create(ID3D12Device *device, UINT w, UINT h, UINT ph, ID3D12Resource* sharedPre=nullptr,NativeShaderCompiler* compiler=nullptr)
    {
        if(root||postWarp||!device||!w||!h||ph<h||UINT64(ph)>=UINT64(h)*2||UINT64(w)*ph>UINT_MAX/16)
            throw std::runtime_error("fast history initialization/geometry contract");
        width=w; height=h; processingHeight=ph;
        postWarp=Buffer(device,UINT64(w)*ph*16);
        previousRaw=Buffer(device,UINT64(w)*ph*16);
        previousModel=Buffer(device,UINT64(w)*ph*16);
        if(sharedPre) {
            // The bridge owns the HIP mapping; this extra reference survives
            // every recording lease. COMMON is the D3D12/HIP handoff boundary.
            auto d=sharedPre->GetDesc();
            if(d.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||d.Width<UINT64(w)*ph*16||
               !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))throw std::runtime_error("history shared pre buffer");
            preWarp=sharedPre;preWarp->AddRef();preState=D3D12_RESOURCE_STATE_COMMON;
        } else preWarp=Buffer(device,UINT64(w)*ph*16);
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
        D3D12_ROOT_PARAMETER params[12]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={1,&range};
        for(UINT i=0;i<4;++i) {params[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; params[i+1].Descriptor.ShaderRegister=i+2;}
        for(UINT i=0;i<4;++i) {params[i+5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; params[i+5].Descriptor.ShaderRegister=i;}
        params[9].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; params[9].Descriptor.ShaderRegister=0;
        params[10].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; params[10].Descriptor.ShaderRegister=6;
        params[11].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; params[11].Descriptor.ShaderRegister=4;
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=12; desc.pParameters=params;
        ID3DBlob *blob=nullptr,*error=nullptr;
        HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
        if(error) error->Release(); Check(hr,"root serialize");
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));
        blob->Release(); Check(hr,"root create");
        for(unsigned i=0;i<2;++i)
        {
            blob=nullptr; error=nullptr;
            hr=NativeCompileShaderBlob(kShader,sizeof(kShader)-1,"NativeFastHistory",nullptr,nullptr,
                                       i?"Finish":"Reproject",&blob,&error,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,compiler);
            std::string diagnostic;
            if(error) {diagnostic.assign(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()); error->Release();}
            if(FAILED(hr)) throw std::runtime_error("temporal shader: "+diagnostic);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature=root;
            pso.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            hr=device->CreateComputePipelineState(&pso,IID_PPV_ARGS(i?&finish:&reproject));
            blob->Release(); Check(hr,"pipeline");
        }
    }
    static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Binding(ID3D12Device* device,ID3D12Resource* motion,ID3D12Resource* depth)
    {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"history binding");
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels=1;
        sv.Format=MotionFormat(motion->GetDesc().Format);device->CreateShaderResourceView(motion,&sv,handle);
        handle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        sv.Format=DepthFormat(depth->GetDesc().Format);device->CreateShaderResourceView(depth,&sv,handle);
        return heap;
    }
    void RecordInputs(ID3D12GraphicsCommandList *cmd,ID3D12Resource *raw,ID3D12Resource *output,
                      ID3D12Resource *motion,ID3D12Resource *depth,D3D12_RESOURCE_STATES ms,D3D12_RESOURCE_STATES ds,
                      const Parameters &p, ID3D12DescriptorHeap* heap, D3D12_GPU_VIRTUAL_ADDRESS control)
    {
        // Only mip 0, array slice 0, plane 0 is exposed by these SRVs. Other
        // mips and the stencil plane can be in different states in the game.
        Transition(cmd,motion,ms,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,0);
        Transition(cmd,depth,ds,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,0);
        Transition(cmd,preWarp,preState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(cmd,postWarp,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Keep each dispatch axis below D3D12's 65535-group limit, including 4K.
        Bind(cmd,raw,output,p,heap,control); cmd->SetPipelineState(reproject); cmd->Dispatch((width+63)/64,processingHeight,1);
        Transition(cmd,preWarp,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,preState);
        Transition(cmd,postWarp,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmd,motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ms,0);
        Transition(cmd,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ds,0);
    }
    void RecordOutputs(ID3D12GraphicsCommandList *cmd,ID3D12Resource *raw,ID3D12Resource *output,
                       ID3D12Resource *depth,D3D12_RESOURCE_STATES ds,const Parameters &p, ID3D12DescriptorHeap* heap, D3D12_GPU_VIRTUAL_ADDRESS control)
    {
        Transition(cmd,depth,ds,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,0);
        for(auto *r:{previousRaw,previousModel,output}) Transition(cmd,r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Bind(cmd,raw,output,p,heap,control); cmd->SetPipelineState(finish); cmd->Dispatch((width+63)/64,processingHeight,1);
        for(auto *r:{previousRaw,previousModel,output}) Transition(cmd,r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmd,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ds,0);
    }
    ID3D12Resource *Warped() const { return preWarp; }
    ID3D12Resource *PostWarped() const { return postWarp; }
};

struct LowFrequencyParameters
{
    UINT width{}, height{}, processingHeight{}, past{};
    UINT motionWidth{1}, motionHeight{1}, motion{}, enhance{};
    float scaleX{}, scaleY{}, memory{0.85f}, strength{1.0f};
    UINT reserved[4]{};
};
static_assert(sizeof(LowFrequencyParameters)==64);

class LowFrequency
{
    ID3D12Resource *difference{}, *observed{}, *observationGuide{};
    ID3D12Resource *priorBand{}, *priorGuide{}, *nextBand{}, *nextGuide{};
    ID3D12RootSignature *root{};
    ID3D12PipelineState *extract{}, *observe{}, *update{};
    ID3D12DescriptorHeap *nullMotionHeap{};
    UINT width{}, height{}, processingHeight{};

    static ID3D12Resource *Buffer(ID3D12Device *device, UINT64 bytes)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes; desc.Height=1;
        desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource *r=nullptr;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r)),"low-frequency buffer");
        return r;
    }
    void Bind(ID3D12GraphicsCommandList *cmd, ID3D12Resource *raw, ID3D12Resource *output, ID3D12DescriptorHeap *heap, D3D12_GPU_VIRTUAL_ADDRESS control)
    {
        ID3D12DescriptorHeap *bindHeap = heap ? heap : nullMotionHeap;
        cmd->SetDescriptorHeaps(1,&bindHeap);
        cmd->SetComputeRootSignature(root);
        cmd->SetComputeRootDescriptorTable(0,bindHeap->GetGPUDescriptorHandleForHeapStart());
        ID3D12Resource *srvs[]={raw,difference,observed,observationGuide,priorBand,priorGuide};
        for(UINT i=0;i<6;++i) cmd->SetComputeRootShaderResourceView(1+i,srvs[i]->GetGPUVirtualAddress());
        ID3D12Resource *uavs[]={difference,observed,observationGuide,nextBand,nextGuide,output};
        for(UINT i=0;i<6;++i) cmd->SetComputeRootUnorderedAccessView(7+i,uavs[i]->GetGPUVirtualAddress());
        cmd->SetComputeRootConstantBufferView(13,control);
    }
public:
    LowFrequency()=default;
    LowFrequency(const LowFrequency&)=delete;
    ~LowFrequency()
    {
        for(auto *r:{difference,observed,observationGuide,priorBand,priorGuide,nextBand,nextGuide}) if(r) r->Release();
        if(nullMotionHeap) nullMotionHeap->Release();
        if(root) root->Release();
        for(auto *p:{extract,observe,update}) if(p) p->Release();
    }
    bool Matches(UINT w,UINT h,UINT ph) const { return width==w && height==h && processingHeight==ph; }
    void Create(ID3D12Device *device, UINT w, UINT h, UINT ph, NativeShaderCompiler* compiler=nullptr)
    {
        if(root||difference||!device||!w||!h||ph<h||UINT64(ph)>=UINT64(h)*2||UINT64(w)*ph>UINT_MAX/16)
            throw std::runtime_error("low-frequency initialization/geometry contract");
        width=w; height=h; processingHeight=ph;
        const UINT64 n=UINT64(w)*h, coarse=UINT64((w+1)/2)*((h+1)/2);
        difference=Buffer(device,n*16);
        observed=Buffer(device,coarse*8);
        observationGuide=Buffer(device,coarse*8);
        priorBand=Buffer(device,n*8);
        priorGuide=Buffer(device,n*8);
        nextBand=Buffer(device,n*8);
        nextGuide=Buffer(device,n*8);
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&nullMotionHeap)),"low-frequency null heap");
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format=DXGI_FORMAT_R32G32_FLOAT;
        sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(nullptr,&sv,nullMotionHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        D3D12_ROOT_PARAMETER params[14]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={1,&range};
        for(UINT i=0;i<6;++i) {
            params[1+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; params[1+i].Descriptor.ShaderRegister=1+i;
            params[7+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; params[7+i].Descriptor.ShaderRegister=i;
        }
        params[13].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV; params[13].Descriptor.ShaderRegister=0;
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=14; desc.pParameters=params;
        ID3DBlob *blob=nullptr,*error=nullptr;
        HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
        if(error) error->Release(); Check(hr,"low-frequency root serialize");
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));
        blob->Release(); Check(hr,"low-frequency root create");
        const char* entries[]={"Extract","Observe","Update"};
        ID3D12PipelineState** targets[]={&extract,&observe,&update};
        for(unsigned i=0;i<3;++i)
        {
            blob=nullptr; error=nullptr;
            hr=NativeCompileShaderBlob(NativeLowFrequencyShader,sizeof(NativeLowFrequencyShader)-1,"IndependentLowFrequency",nullptr,nullptr,
                                       entries[i],&blob,&error,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,compiler);
            std::string diagnostic;
            if(error) {diagnostic.assign(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()); error->Release();}
            if(FAILED(hr)) throw std::runtime_error("low-frequency shader: "+diagnostic);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature=root;
            pso.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            hr=device->CreateComputePipelineState(&pso,IID_PPV_ARGS(targets[i]));
            blob->Release(); Check(hr,"low-frequency pipeline");
        }
    }
    static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Binding(ID3D12Device* device,ID3D12Resource* motion)
    {
        if(!motion) return {};
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"low-frequency motion binding");
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels=1;
        sv.Format=MotionFormat(motion->GetDesc().Format);device->CreateShaderResourceView(motion,&sv,handle);
        return heap;
    }
    ID3D12DescriptorHeap* NullBinding() const { return nullMotionHeap; }
    void RecordOutputs(ID3D12GraphicsCommandList *cmd,ID3D12Resource *raw,ID3D12Resource *output,
                       D3D12_RESOURCE_STATES rawRest,ID3D12Resource *motion,D3D12_RESOURCE_STATES motionRest,
                       ID3D12DescriptorHeap* heap,D3D12_GPU_VIRTUAL_ADDRESS control)
    {
        Transition(cmd,raw,rawRest,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(motion) Transition(cmd,motion,motionRest,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,0);
        Transition(cmd,output,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(cmd,difference,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Bind(cmd,raw,output,heap,control);
        cmd->SetPipelineState(extract); cmd->Dispatch((width+7)/8,(height+7)/8,1);
        Transition(cmd,difference,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(auto *r:{observed,observationGuide}) Transition(cmd,r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(observe); cmd->Dispatch(((width+1)/2+7)/8,((height+1)/2+7)/8,1);
        for(auto *r:{observed,observationGuide}) Transition(cmd,r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(auto *r:{nextBand,nextGuide}) Transition(cmd,r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(update); cmd->Dispatch((width+7)/8,(height+7)/8,1);
        Transition(cmd,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(auto *r:{nextBand,nextGuide}) Transition(cmd,r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        for(auto *r:{priorBand,priorGuide}) Transition(cmd,r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(priorBand,nextBand);
        cmd->CopyResource(priorGuide,nextGuide);
        for(auto *r:{nextBand,nextGuide}) Transition(cmd,r,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(auto *r:{priorBand,priorGuide}) Transition(cmd,r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(motion) Transition(cmd,motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,motionRest,0);
        Transition(cmd,raw,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,rawRest);
    }
};
}

