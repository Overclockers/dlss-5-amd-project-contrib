// Exercise the shipped SPIR-V, without model weights: hue, unchanged SDR, alpha and dispatch bounds.
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <nrvk.hpp>

using Pixel = std::array<float, 4>;

static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

static float Decode(float x)
{
    return x <= .04045f ? x / 12.92f : std::pow((x + .055f) / 1.055f, 2.4f);
}

static void SameHue(const Pixel& encoded, const Pixel& source)
{
    float decoded[3] = { Decode(encoded[0]), Decode(encoded[1]), Decode(encoded[2]) };
    const float a = std::max({ decoded[0], decoded[1], decoded[2] });
    const float b = std::max({ source[0], source[1], source[2] });
    for (int c = 0; c != 3; ++c)
        Require(std::abs(decoded[c] / a - source[c] / b) < 2e-5f, "HDR encoding changed RGB ratios");
}

struct Resources
{
    nrvk::Context& ctx;
    std::vector<nrvk::Context::Image> images;
    nrvk::Kernel kernel;
    explicit Resources(nrvk::Context& c) : ctx(c) { images.reserve(4); }
    ~Resources()
    {
        if (kernel.device) kernel.destroy();
        for (auto& im : images) ctx.destroy(im);
    }
    auto& Image(uint32_t w, uint32_t h, bool sampled = false)
    {
        images.push_back(ctx.image(w, h, VK_FORMAT_R32G32B32A32_SFLOAT, sampled));
        return images.back();
    }
    template<class Push> void Run(const Push& push, uint32_t width, uint32_t height)
    {
        ctx.one_shot([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, kernel.layout, 0, 1,
                                    &kernel.set, 0, nullptr);
            vkCmdPushConstants(cmd, kernel.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
            vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
        });
    }
};

static const Pixel colors[] = {
    { 4.f, 1.f, .25f, .375f }, { .25f, 4.f, 1.f, .5f }, { 1.f, .25f, 4.f, .625f },
    { 16.f, 4.f, 1.f, .75f }, { .18f, .09f, .045f, 1.f }, { 0.f, 0.f, 0.f, 0.f },
    { -1.f, .25f, .125f, .25f }
};

static void Encode(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    constexpr uint32_t w = 19, h = 11, validW = 17, validH = 9;
    Resources r(ctx);
    auto& proxy = r.Image(w, h);
    auto& keep = r.Image(w, h);
    r.kernel.create(ctx, (shaders / "runtime_encode.spv").string(), {}, 16, { &proxy, &keep });
    std::vector<Pixel> input(w * h), output(w * h), kept(w * h), sentinel(w * h, { -9.f, -9.f, -9.f, -9.f });
    for (size_t i = 0; i < input.size(); ++i) input[i] = colors[i % std::size(colors)];
    for (float white : { .25f, 1.f, 4.f, 0.f })
    {
        ctx.upload(proxy, input.data(), input.size() * sizeof(Pixel));
        ctx.upload(keep, sentinel.data(), sentinel.size() * sizeof(Pixel));
        struct { uint32_t w, h; float white, knee; } push { validW, validH, white, .75f };
        r.Run(push, w, h);
        ctx.download(proxy, output.data(), output.size() * sizeof(Pixel));
        ctx.download(keep, kept.data(), kept.size() * sizeof(Pixel));
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x)
        {
            const size_t i = y * w + x;
            if (x >= validW || y >= validH)
            {
                Require(output[i] == input[i] && kept[i] == sentinel[i], "encode wrote outside the valid extent");
                continue;
            }
            Require(kept[i] == input[i] && output[i][3] == input[i][3], "encode changed original/alpha");
            for (int c = 0; c != 3; ++c)
                Require(std::isfinite(output[i][c]) && output[i][c] >= 0.f && output[i][c] <= 1.000001f,
                        "encode produced invalid or unbounded output");
            Pixel nonnegative = input[i];
            for (int c = 0; c != 3; ++c) nonnegative[c] = std::max(0.f, nonnegative[c]);
            if (nonnegative[0] + nonnegative[1] + nonnegative[2] > 0.f) SameHue(output[i], nonnegative);
            // Below the knee and peak limit the operation remains the ordinary sRGB transfer.
            const float norm = std::max(white, 1e-4f);
            if (std::max({nonnegative[0], nonnegative[1], nonnegative[2]}) / norm <= .75f)
                for (int c = 0; c != 3; ++c)
                    Require(std::abs(Decode(output[i][c]) - nonnegative[c] / norm) < 2e-6f,
                            "encode changed ordinary brightness");
        }
    }
}

static void Transfer(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    constexpr uint32_t w = 19, h = 11;
    for (bool scaled : { false, true })
    for (uint32_t mode : { 0u, 1u })
    for (bool compact : {false, true})
    {
        const uint32_t mw = scaled ? 2 : w, mh = scaled ? 2 : h;
        Resources r(ctx);
        auto& model = r.Image(mw, mh);
        auto& shown = r.Image(mw, mh, true);
        auto& keep = r.Image(w, h, true);
        auto& answer = r.Image(w, h);
        ctx.transition(keep, VK_IMAGE_LAYOUT_GENERAL);
        auto alias = keep; alias.sampler = VK_NULL_HANDLE;
        r.kernel.create(ctx, (shaders / "runtime_transfer.spv").string(), {}, 48,
                        { &model, &shown, &keep, compact ? &alias : &answer, &answer });
        for (const Pixel& source : colors)
        {
            if (source[0] < 0.f) continue; // transfer has its own gamut handling, outside this change
            Pixel proxy = source;
            // Any common positive normalization cancels in the no-edit transfer; peak-normalize HDR.
            const float peak = std::max({ 1.f, source[0], source[1], source[2] });
            for (int c = 0; c != 3; ++c)
            {
                const float v = source[c] / peak;
                proxy[c] = v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - .055f;
            }
            std::vector<Pixel> pixels(mw * mh, proxy), original(w * h, source), output(w * h);
            ctx.upload(model, pixels.data(), pixels.size() * sizeof(Pixel));
            ctx.upload(shown, pixels.data(), pixels.size() * sizeof(Pixel));
            ctx.upload(keep, original.data(), original.size() * sizeof(Pixel));
            struct { uint32_t w, h, mw, mh, passthrough; float detail, colour, guard, white; uint32_t mode, flags; float sigma; }
                push { w, h, mw, mh, 0, 1.f, 1.f, 4.f, 1.f, mode, 0, 1.f };
            r.Run(push, w, h);
            ctx.download(compact ? keep : answer, output.data(), output.size() * sizeof(Pixel));
            for (const Pixel& p : output)
            {
                Require(p[3] == source[3], "transfer changed alpha");
                for (int c = 0; c != 3; ++c)
                    Require(std::isfinite(p[c]) && std::abs(p[c] - source[c]) < .001f,
                            "no-edit transfer changed colour (full or reduced model size)");
            }
        }
    }
}

// Asymmetric sizes cover partial workgroups and clamped 4x4 taps. Compare the
// actual in-place binding against a separate output, including high contrast.
static void Composition(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    constexpr uint32_t w=137, h=79, mw=16, mh=10;
    std::vector<Pixel> original(w*h), proxy(mw*mh), model(mw*mh);
    auto run = [&](uint32_t mode, bool compact, uint32_t linear, float detail, float colour) {
        Resources r(ctx);
        auto& m=r.Image(mw,mh); auto& s=r.Image(mw,mh,true);
        auto& keep=r.Image(w,h,true); auto& answer=r.Image(w,h);
        ctx.transition(keep,VK_IMAGE_LAYOUT_GENERAL);
        auto alias=keep;alias.sampler=VK_NULL_HANDLE;
        r.kernel.create(ctx,(shaders/"runtime_transfer.spv").string(),{},48,
                        {&m,&s,&keep,compact?&alias:&answer,&answer});
        ctx.upload(m,model.data(),model.size()*sizeof(Pixel));
        ctx.upload(s,proxy.data(),proxy.size()*sizeof(Pixel));
        ctx.upload(keep,original.data(),original.size()*sizeof(Pixel));
        struct {uint32_t w,h,mw,mh,passthrough;float detail,colour,guard,white;uint32_t mode,flags;float sigma;}
            push{w,h,mw,mh,1u-linear,detail,colour,4.f,1.f,mode,mode==2?2u:0u,1.f};
        r.Run(push,w,h);
        std::vector<Pixel> out(w*h);
        ctx.download(compact?keep:answer,out.data(),out.size()*sizeof(Pixel));
        for(size_t i=0;i<out.size();++i) {
            Require(out[i][3]==original[i][3],"composition changed alpha");
            for(int c=0;c<3;++c) Require(std::isfinite(out[i][c]) && out[i][c]>=0,"invalid composition pixel");
        }
        return out;
    };
    for(uint32_t linear:{0u,1u}) {
        for(size_t i=0;i<original.size();++i) {
            const float x=float(i%w)/w,y=float(i/w)/h;
            original[i]={x*x*(linear?16.f:1.f),y*.7f,((i/7)%2)*.2f,float(i%17)/16.f};
        }
        for(size_t i=0;i<proxy.size();++i) {
            const float x=float(i%mw)/mw,y=float(i/mw)/mh;
            proxy[i]={x*.7f,y*.4f,.1f,1};
            model[i]={x*.6f+.07f,y*.3f+.08f,.08f,1};
        }
        for(uint32_t mode:{0u,1u,2u}) for(float detail:{0.f,1.f,2.f}) {
            auto separate=run(mode,false,linear,detail,1.5f);
            auto compact=run(mode,true,linear,detail,1.5f);
            Require(separate==compact,"in-place transfer differs from separate output");
        }
        model=proxy;
        // Matched residual and lighting edits vanish when model == proxy.
        // Classic deliberately substitutes the enlarged model picture.
        for(uint32_t mode:{1u,2u}) {
            auto out=run(mode,true,linear,1.f,1.f);
            for(size_t i=0;i<out.size();++i) for(int c=0;c<3;++c)
                Require(std::abs(out[i][c]-original[i][c])<.003f,"spatial no-edit invariant failed");
        }
    }
    // A foreground/background edge: the neighbour on the other side must
    // not dominate the local lighting edit.
    for(size_t i=0;i<original.size();++i) {
        float v=i%w<w/2?.1f:.8f; original[i]={v,v,v,.5f};
    }
    for(size_t i=0;i<proxy.size();++i) {
        bool left=i%mw<mw/2; float s=left?.1f:.8f,m=left?.15f:.6f;
        proxy[i]={s,s,s,1};model[i]={m,m,m,1};
    }
    auto matched=run(1,true,0,1,1),edge=run(2,true,0,1,1);
    for(uint32_t x:{w/2-1,w/2}) {
        float target=x<w/2?.15f:.6f;size_t i=(h/2)*w+x;
        Require(std::abs(edge[i][0]-target)<std::abs(matched[i][0]-target),
                "edge-aware mode failed to reject the opposite side");
    }
    std::cout<<"PASS composition: HDR/SDR, in-place byte equality, spatial no-edit, edge separation\n";
}

// Compare actual compute conversions with the driver's reference blit. Include
// every finite half value, signed zero, subnormals and FP32 values near half ties.
static void FloatConversions(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    constexpr uint32_t w=257,h=257;
    Resources r(ctx); r.images.reserve(5);
    r.images.push_back(ctx.image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,false));
    r.images.push_back(ctx.image(w,h,VK_FORMAT_R16G16B16A16_SFLOAT,true));
    r.images.push_back(ctx.image(w,h,VK_FORMAT_R16G16B16A16_SFLOAT,false));
    r.images.push_back(ctx.image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,false));
    r.images.push_back(ctx.image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,false));
    auto& source=r.images[0]; auto& half=r.images[1]; auto& reference=r.images[2];
    auto& unpacked=r.images[3]; auto& unpackReference=r.images[4];
    std::vector<Pixel> values(w*h), actual(w*h), expected(w*h);
    for(size_t i=0;i<values.size()*4;++i) {
        const uint32_t bits=uint32_t(i)%65536, exponent=(bits>>10)&31, mantissa=bits&1023;
        float value=exponent==0 ? std::ldexp(float(mantissa),-24)
                               : std::ldexp(float(1024+mantissa),int(exponent==31?30:exponent)-25);
        if(bits&32768)value=-value;
        if(i>=65536 && exponent>0 && exponent<30) {
            value += std::ldexp(1.f,int(exponent)-26); // midpoint to the next half
            if(i/65536%3) value=std::nextafter(value,i/65536%3==1?INFINITY:-INFINITY);
        }
        values[i/4][i%4]=value;
    }
    ctx.upload(source,values.data(),values.size()*sizeof(Pixel));
    ctx.transition(half,VK_IMAGE_LAYOUT_GENERAL);
    auto halfStorage=half;halfStorage.sampler=VK_NULL_HANDLE;
    r.kernel.create(ctx,(shaders/"runtime_pack_f16.spv").string(),{},8,{&source,&halfStorage});
    const std::array<uint32_t,2> push{w,h};r.Run(push,w,h);
    auto blit=[&](auto& from,auto& to) {
        const auto oldFrom=from.layout,oldTo=to.layout;
        ctx.transition(from,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        ctx.transition(to,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        ctx.one_shot([&](VkCommandBuffer cmd) {
            VkImageBlit area{};area.srcSubresource=area.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            area.srcOffsets[1]=area.dstOffsets[1]={int(w),int(h),1};
            vkCmdBlitImage(cmd,from.handle,from.layout,to.handle,to.layout,1,&area,VK_FILTER_NEAREST);
        });
        ctx.transition(from,oldFrom);ctx.transition(to,oldTo);
    };
    blit(source,reference);
    std::vector<uint16_t> packed(w*h*4),ref(w*h*4);
    ctx.download(half,packed.data(),packed.size()*sizeof(uint16_t));
    ctx.download(reference,ref.data(),ref.size()*sizeof(uint16_t));
    Require(packed==ref,"FP16 compute writeback differs from blit");
    r.kernel.destroy();r.kernel={};ctx.transition(half,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    r.kernel.create(ctx,(shaders/"runtime_unpack_f16.spv").string(),{},8,{&half,&unpacked});
    r.Run(push,w,h);blit(half,unpackReference);
    ctx.download(unpacked,actual.data(),actual.size()*sizeof(Pixel));
    ctx.download(unpackReference,expected.data(),expected.size()*sizeof(Pixel));
    Require(std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(Pixel))==0,
            "FP16 compute input differs from blit");
    std::cout<<"PASS FP16 conversions: blit byte equality, HDR, negatives, alpha, subnormals, ties\n";
}

// NrInlineComputeConvert: a B10G11R11 frame copy unpacked by runtime_unpack_f16 (texelFetch)
// must equal the driver's reference blit into RGBA32F, bit for bit, over every finite 11/10-bit
// channel code (all mantissas and exponents 0..30, so zero, subnormals and the largest finite).
static void R11G11B10Unpack(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    constexpr uint32_t w=257,h=257;
    Resources r(ctx); r.images.reserve(3);
    r.images.push_back(ctx.image(w,h,VK_FORMAT_B10G11R11_UFLOAT_PACK32,true,true,false));
    r.images.push_back(ctx.image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,false));
    r.images.push_back(ctx.image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,false));
    auto& packed=r.images[0]; auto& unpacked=r.images[1]; auto& reference=r.images[2];
    std::vector<uint32_t> bits(w*h);
    for(uint32_t i=0;i<bits.size();++i) {
        // R/G: 11-bit (5e6m), B: 10-bit (5e5m). Walk the codes, skipping exponent 31 (Inf/NaN).
        auto code=[](uint32_t n,uint32_t mbits){ const uint32_t m=n&((1u<<mbits)-1), e=(n>>mbits)%31; return (e<<mbits)|m; };
        const uint32_t rr=code(i,6), gg=code(i*7+3,6), bb=code(i*13+5,5);
        bits[i]=rr|(gg<<11)|(bb<<22);
    }
    ctx.upload(packed,bits.data(),bits.size()*sizeof(uint32_t));
    ctx.transition(packed,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    r.kernel.create(ctx,(shaders/"runtime_unpack_f16.spv").string(),{},8,{&packed,&unpacked});
    const std::array<uint32_t,2> push{w,h};r.Run(push,w,h);
    ctx.transition(packed,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ctx.transition(reference,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    ctx.one_shot([&](VkCommandBuffer cmd) {
        VkImageBlit area{};area.srcSubresource=area.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        area.srcOffsets[1]=area.dstOffsets[1]={int(w),int(h),1};
        vkCmdBlitImage(cmd,packed.handle,packed.layout,reference.handle,reference.layout,1,&area,VK_FILTER_NEAREST);
    });
    ctx.transition(reference,VK_IMAGE_LAYOUT_GENERAL);
    std::vector<Pixel> actual(w*h), expected(w*h);
    ctx.download(unpacked,actual.data(),actual.size()*sizeof(Pixel));
    ctx.download(reference,expected.data(),expected.size()*sizeof(Pixel));
    Require(std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(Pixel))==0,
            "B10G11R11 compute input differs from blit");
    for(const Pixel& p:actual) Require(p[3]==1.f && p[0]>=0.f && p[1]>=0.f && p[2]>=0.f && std::isfinite(p[0]+p[1]+p[2]),
                                       "B10G11R11 unpack produced invalid pixel");
    std::cout<<"PASS B10G11R11 compute input: blit bit equality, zero, subnormals, max finite, alpha 1\n";
}

int main(int argc, char** argv)
{
    nrvk::Context ctx;
    try
    {
        std::cout << std::unitbuf;
        Require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--fp16-only"),
                "usage: hdr_shader <shaders/runtime> [--fp16-only]");
        ctx.create();
        std::cout << "GPU: " << ctx.gpu_name << '\n';
        FloatConversions(ctx, argv[1]);
        R11G11B10Unpack(ctx, argv[1]);
        if (argc == 2) {
            Encode(ctx, argv[1]);
            Transfer(ctx, argv[1]);
            Composition(ctx, argv[1]);
        }
        ctx.destroy();
        if (argc == 2)
            std::cout << "PASS Mochizuki HDR encode/transfer: hue, SDR, alpha, bounds, scaled/unscaled\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        if (ctx.device) ctx.destroy();
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
