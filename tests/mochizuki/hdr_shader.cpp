// Exercise the shipped SPIR-V, without model weights: hue, unchanged SDR, alpha and dispatch bounds.
#include <algorithm>
#include <array>
#include <cstring>
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

// The encoder keeps the raw frame, but the linear composition reference must
// match OptiScaler's nonnegative RGB. Small negative HDR samples used to make
// the luminance-ratio correction amplify a bounded model answer into hot pixels.
static void NegativeHdrReference(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    constexpr uint32_t w = 19, h = 11;
    const Pixel sources[] = {
        { -0.00074625f, -0.00170135f, -0.00724411f, .375f },
        { .001f, -.004f, .0005f, .25f }, { -.004f, .0005f, .002f, .5f },
        { .003f, -.00025f, .001f, .75f }, { -.5f, .25f, .125f, .875f },
        { 0.f, 0.f, 0.f, 0.f }, { .18f, .09f, .045f, 1.f }
    };
    for (bool scaled : { false, true })
    for (float white : { .25f, 1.f, 4.f })
    for (const Pixel& source : sources)
    {
        const uint32_t mw = scaled ? 7 : w, mh = scaled ? 5 : h;
        std::vector<Pixel> proxy(mw * mh, source);
        {
            Resources e(ctx);
            auto& p = e.Image(mw, mh); auto& k = e.Image(mw, mh);
            ctx.upload(p, proxy.data(), proxy.size() * sizeof(Pixel));
            e.kernel.create(ctx, (shaders / "runtime_encode.spv").string(), {}, 16, { &p, &k });
            struct { uint32_t w, h; float white, knee; } push { mw, mh, white, .75f };
            e.Run(push, mw, mh);
            ctx.download(p, proxy.data(), proxy.size() * sizeof(Pixel));
        }
        auto model = proxy;
        for (auto& p : model)
        {
            p[0] = std::clamp(p[0] + .015f, 0.f, 1.f);
            p[1] = std::clamp(p[1] - .005f, 0.f, 1.f);
            p[2] = std::clamp(p[2] + .01f, 0.f, 1.f);
        }
        Resources r(ctx);
        auto& m = r.Image(mw, mh); auto& s = r.Image(mw, mh, true);
        auto& k = r.Image(w, h, true); auto& a = r.Image(w, h);
        ctx.upload(m, model.data(), model.size() * sizeof(Pixel));
        ctx.upload(s, proxy.data(), proxy.size() * sizeof(Pixel));
        r.kernel.create(ctx, (shaders / "runtime_transfer.spv").string(), {}, 48, { &m, &s, &k, &a, &a });
        for (uint32_t mode : { 0u, 1u, 2u })
        {
            struct { uint32_t w, h, mw, mh, passthrough; float detail, colour, guard, white;
                     uint32_t mode, flags; float sigma; }
                push { w, h, mw, mh, 0, 1.f, 1.f, 2.f, white, mode, 0, 1.f };
            auto run = [&](bool nonnegative) {
                Pixel reference = source;
                if (nonnegative) for (int c = 0; c != 3; ++c) reference[c] = std::max(reference[c], 0.f);
                std::vector<Pixel> original(w * h, reference), output(w * h);
                ctx.upload(k, original.data(), original.size() * sizeof(Pixel));
                r.Run(push, w, h);
                ctx.download(a, output.data(), output.size() * sizeof(Pixel));
                return output;
            };
            const auto actual = run(false), reference = run(true);
            Require(actual == reference, "negative HDR reference differs from OptiScaler's nonnegative reference");
            for (const auto& p : actual)
            {
                Require(p[3] == source[3], "negative HDR composition changed alpha");
                for (int c = 0; c != 3; ++c)
                    Require(std::isfinite(p[c]) && p[c] >= 0.f && p[c] < 8.f,
                            "negative HDR composition generated an extreme or invalid colour");
            }
        }
    }
    std::cout << "PASS negative HDR reference: encoded input, native/scaled, all transfer modes, white points, alpha\n";
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

// B10G11R11 compute write-back: runtime_pack_r11 (imageStore through r11f_g11f_b10f) must equal the
// driver's reference blit RGBA32F -> B10G11R11, bit for bit: every finite code, the midpoints between
// neighbouring codes (rounding ties), values above the largest finite, negatives, and random floats.
static void R11G11B10Pack(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(ctx.physical, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) { std::cout<<"SKIP B10G11R11 pack: no storage image support\n"; return; }
    constexpr uint32_t w=256,h=256;
    Resources r(ctx); r.images.reserve(3);
    r.images.push_back(ctx.image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,false));
    r.images.push_back(ctx.image(w,h,VK_FORMAT_B10G11R11_UFLOAT_PACK32,false));
    r.images.push_back(ctx.image(w,h,VK_FORMAT_B10G11R11_UFLOAT_PACK32,false));
    auto& source=r.images[0]; auto& packed=r.images[1]; auto& reference=r.images[2];
    auto decode=[](uint32_t c,uint32_t mbits){ const uint32_t m=c&((1u<<mbits)-1), e=c>>mbits;
        return e==0 ? std::ldexp(float(m),-14-int(mbits)) : std::ldexp(1.f+float(m)/float(1u<<mbits),int(e)-15); };
    std::vector<Pixel> in(w*h); uint32_t rng=12345u; auto next=[&]{ rng=rng*1664525u+1013904223u; return rng; };
    for(uint32_t i=0;i<in.size();++i) {
        const uint32_t kind=i%4, n=i/4;
        auto val=[&](uint32_t mbits,uint32_t salt)->float {
            const uint32_t codes=31u<<mbits, c=(n*salt+salt)%codes;
            if(kind==0) return decode(c,mbits);                                     // exact codes
            if(kind==1) return c+1<codes ? 0.5f*(decode(c,mbits)+decode(c+1,mbits)) : 70000.f; // ties
            if(kind==2) { const float f=std::ldexp(float(next()%1000000)/1000000.f,int(next()%34)-20); return f; } // random
            const uint32_t k=n%3; return k==0 ? -decode(c,mbits) : k==1 ? 65024.f+float(n%2000) : 0.f; // negative / above max
        };
        in[i]={val(6,1),val(6,7),val(5,13),1.f};
    }
    ctx.upload(source,in.data(),in.size()*sizeof(Pixel));
    ctx.transition(source,VK_IMAGE_LAYOUT_GENERAL); ctx.transition(packed,VK_IMAGE_LAYOUT_GENERAL);
    r.kernel.create(ctx,(shaders/"runtime_pack_r11.spv").string(),{},8,{&source,&packed});
    const std::array<uint32_t,2> push{w,h};r.Run(push,w,h);
    ctx.transition(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ctx.transition(reference,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    ctx.one_shot([&](VkCommandBuffer cmd) {
        VkImageBlit area{};area.srcSubresource=area.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        area.srcOffsets[1]=area.dstOffsets[1]={int(w),int(h),1};
        vkCmdBlitImage(cmd,source.handle,source.layout,reference.handle,reference.layout,1,&area,VK_FILTER_NEAREST);
    });
    ctx.transition(packed,VK_IMAGE_LAYOUT_GENERAL); ctx.transition(reference,VK_IMAGE_LAYOUT_GENERAL);
    std::vector<uint32_t> actual(w*h), expected(w*h);
    ctx.download(packed,actual.data(),actual.size()*4); ctx.download(reference,expected.data(),expected.size()*4);
    uint32_t diff[4]{};
    for(uint32_t i=0;i<actual.size();++i) if(actual[i]!=expected[i]) ++diff[i%4];
    std::cout<<"B10G11R11 pack vs blit differing texels: exact="<<diff[0]<<" ties="<<diff[1]<<" random="<<diff[2]<<" neg/over="<<diff[3]<<'\n';
    Require(diff[0]+diff[1]+diff[2]+diff[3]==0, "B10G11R11 compute write-back differs from blit");
    std::cout<<"PASS B10G11R11 compute write-back: blit bit equality, codes, ties, random, negative, above max\n";
}

// NrInlineComputeConvert, 32-bit UNORM formats: runtime_unpack_f16 (input) must equal the reference blit
// into RGBA32F bit for bit, and the per-format pack (write-back) must equal the reference blit
// RGBA32F -> format. 8-bit: exact on k/255 answers (what the alpha pass leaves), at most 1 LSB elsewhere.
// 10-bit: at most 1 LSB (blit rounding is implementation-defined); the counts are printed.
static void PackedUnorm(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    struct Case { VkFormat format; const char* pack; bool ten; const char* name; };
    const Case cases[] = {
        {VK_FORMAT_R8G8B8A8_UNORM, "runtime_pack_u8.spv", false, "R8G8B8A8_UNORM"},
        {VK_FORMAT_B8G8R8A8_UNORM, "runtime_pack_u8_bgra.spv", false, "B8G8R8A8_UNORM"},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "runtime_pack_u10.spv", true, "A2B10G10R10_UNORM"},
        {VK_FORMAT_A2R10G10B10_UNORM_PACK32, "runtime_pack_u10_bgra.spv", true, "A2R10G10B10_UNORM"},
    };
    constexpr uint32_t w=256,h=256;
    for (const Case& c : cases) {
        VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(ctx.physical, c.format, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) != need) { std::cout<<"SKIP "<<c.name<<": no blit/sampled support\n"; continue; }
        Resources r(ctx); r.images.reserve(5);
        r.images.push_back(ctx.image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,false));   // answer
        r.images.push_back(ctx.image(w,h,VK_FORMAT_R32_UINT,false));              // packed
        r.images.push_back(ctx.image(w,h,c.format,true,true,false));              // reference (blit dst) / unpack source
        r.images.push_back(ctx.image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,false));   // unpacked
        r.images.push_back(ctx.image(w,h,VK_FORMAT_R32G32B32A32_SFLOAT,false));   // unpack reference
        auto& answer=r.images[0]; auto& packed=r.images[1]; auto& ref=r.images[2]; auto& unpacked=r.images[3]; auto& uref=r.images[4];
        std::vector<Pixel> in(w*h); uint32_t rng=777u; auto next=[&]{ rng=rng*1664525u+1013904223u; return rng>>8; };
        const float steps = c.ten ? 1023.f : 255.f, asteps = c.ten ? 3.f : 255.f;
        for (uint32_t i=0;i<in.size();++i) {
            const bool exact = (i & 1u) == 0;
            auto v=[&](float n){ return exact ? float(next()%uint32_t(n+1))/n : float(next()%1000001u)/1000000.f; };
            in[i]={v(steps),v(steps),v(steps),v(asteps)};
        }
        ctx.upload(answer,in.data(),in.size()*sizeof(Pixel));
        ctx.transition(answer,VK_IMAGE_LAYOUT_GENERAL); ctx.transition(packed,VK_IMAGE_LAYOUT_GENERAL);
        r.kernel.create(ctx,(shaders/c.pack).string(),{},8,{&answer,&packed});
        const std::array<uint32_t,2> push{w,h}; r.Run(push,w,h);
        ctx.transition(answer,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); ctx.transition(ref,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit area{}; area.srcSubresource=area.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        area.srcOffsets[1]=area.dstOffsets[1]={int(w),int(h),1};
        ctx.one_shot([&](VkCommandBuffer cmd){ vkCmdBlitImage(cmd,answer.handle,answer.layout,ref.handle,ref.layout,1,&area,VK_FILTER_NEAREST); });
        ctx.transition(packed,VK_IMAGE_LAYOUT_GENERAL); ctx.transition(ref,VK_IMAGE_LAYOUT_GENERAL);
        std::vector<uint32_t> got(w*h), want(w*h);
        ctx.download(packed,got.data(),got.size()*4); ctx.download(ref,want.data(),want.size()*4);
        uint32_t exactDiff=0, otherDiff=0, maxLsb=0;
        for (uint32_t i=0;i<got.size();++i) if (got[i]!=want[i]) {
            ((i&1u)==0 ? exactDiff : otherDiff)++;
            const int bits[4] = {c.ten?10:8,c.ten?10:8,c.ten?10:8,c.ten?2:8};
            int shift=0;
            for (int k=0;k<4;++k) { const uint32_t mask=(1u<<bits[k])-1u;
                const int a=int((got[i]>>shift)&mask), b=int((want[i]>>shift)&mask);
                maxLsb=std::max<uint32_t>(maxLsb,uint32_t(std::abs(a-b))); shift+=bits[k]; }
        }
        std::cout<<c.name<<" pack vs blit: exact-input diff="<<exactDiff<<" other diff="<<otherDiff<<" max LSB="<<maxLsb<<'\n';
        Require(maxLsb<=1, "compute write-back differs from blit by more than 1 LSB");
        Require(c.ten || exactDiff==0, "8-bit compute write-back differs from blit on k/255 answers");
        // Input side: the blit's own bits, unpacked both ways.
        ctx.transition(ref,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); ctx.transition(unpacked,VK_IMAGE_LAYOUT_GENERAL);
        Resources u(ctx); u.kernel.create(ctx,(shaders/"runtime_unpack_f16.spv").string(),{},8,{&ref,&unpacked}); u.Run(push,w,h);
        ctx.transition(ref,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); ctx.transition(uref,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        ctx.one_shot([&](VkCommandBuffer cmd){ vkCmdBlitImage(cmd,ref.handle,ref.layout,uref.handle,uref.layout,1,&area,VK_FILTER_NEAREST); });
        ctx.transition(unpacked,VK_IMAGE_LAYOUT_GENERAL); ctx.transition(uref,VK_IMAGE_LAYOUT_GENERAL);
        std::vector<Pixel> ua(w*h), ub(w*h);
        ctx.download(unpacked,ua.data(),ua.size()*sizeof(Pixel)); ctx.download(uref,ub.data(),ub.size()*sizeof(Pixel));
        Require(std::memcmp(ua.data(),ub.data(),ua.size()*sizeof(Pixel))==0, "UNORM compute input differs from blit");
        std::cout<<"PASS "<<c.name<<" compute input (bit equal) and write-back\n";
    }
}

// NrInlineComputeMotion: runtime_mv_rescale_* (LINEAR, clamp-to-edge sampler) against the LINEAR blit it replaces, for
// a smaller, a larger and a padded source (valid extent inside a larger image whose edge repeats).
static uint16_t HalfBits(float f)
{
    uint32_t x; std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u; int e = int((x >> 23) & 0xff) - 127 + 15; uint32_t m = x & 0x7fffffu;
    if ((x & 0x7fffffffu) == 0) return uint16_t(sign);
    Require(e > 0 && e < 31 && (m & 0x1fffu) == 0, "fixture value not exact in half");
    return uint16_t(sign | (uint32_t(e) << 10) | (m >> 13));
}
static float HalfFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ffu;
    uint32_t x = e == 0 ? sign : sign | ((e - 15 + 127) << 23) | (m << 13);
    if (e == 0 && m) { float v = std::ldexp(float(m), -24); return (h & 0x8000u) ? -v : v; }
    float f; std::memcpy(&f, &x, 4); return f;
}
static void MotionRescale(nrvk::Context& ctx, const std::filesystem::path& shaders)
{
    struct Case { uint32_t sw, sh, vw, vh, dw, dh; const char* name; };
    const Case cases[] = { {256,160,256,160,192,120,"smaller"}, {160,100,160,100,240,150,"larger"},
                           {160,100,150,90,232,145,"padded"} };
    struct Fmt { VkFormat format; bool half; uint32_t ch; const char* module; const char* name; };
    const Fmt fmts[] = { {VK_FORMAT_R16G16_SFLOAT,true,2,"runtime_mv_rescale_rg16f.spv","RG16F"},
                         {VK_FORMAT_R32G32_SFLOAT,false,2,"runtime_mv_rescale_rg32f.spv","RG32F"},
                         {VK_FORMAT_R16G16B16A16_SFLOAT,true,4,"runtime_mv_rescale_rgba16f.spv","RGBA16F"},
                         {VK_FORMAT_R32G32B32A32_SFLOAT,false,4,"runtime_mv_rescale_rgba32f.spv","RGBA32F"} };
    for (const Fmt& fm : fmts) {
        const VkFormat format = fm.format; const bool half = fm.half; const uint32_t ch = fm.ch; const char* module = fm.module;
        for (const Case& c : cases) {
            Resources r(ctx); r.images.reserve(3);
            r.images.push_back(ctx.image(c.sw,c.sh,format,true,true,false));   // source (sampled + blit src)
            r.images.push_back(ctx.image(256,160,format,false));               // blit reference
            r.images.push_back(ctx.image(256,160,format,false));               // compute result
            auto& src=r.images[0]; auto& ref=r.images[1]; auto& got=r.images[2];
            vkDestroySampler(ctx.device, src.sampler, nullptr);
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            Require(vkCreateSampler(ctx.device, &si, nullptr, &src.sampler) == VK_SUCCESS, "linear sampler");
            // Values k/16 in [-64, 64): exact in both formats. Past the valid extent the edge repeats (Pad).
            std::vector<float> v(size_t(c.sw)*c.sh*ch); uint32_t rng=4242u; auto next=[&]{ rng=rng*1664525u+1013904223u; return rng>>8; };
            for (uint32_t y=0;y<c.sh;++y) for (uint32_t x=0;x<c.sw;++x) for (uint32_t k=0;k<ch;++k) {
                const uint32_t sx=std::min(x,c.vw-1), sy=std::min(y,c.vh-1);
                if (sx==x && sy==y) v[(size_t(y)*c.sw+x)*ch+k]=float(int(next()%2048u)-1024)/16.f;
                else v[(size_t(y)*c.sw+x)*ch+k]=v[(size_t(sy)*c.sw+sx)*ch+k];
            }
            ctx.transition(src,VK_IMAGE_LAYOUT_GENERAL);
            if (half) { std::vector<uint16_t> h(v.size()); for (size_t i=0;i<v.size();++i) h[i]=HalfBits(v[i]); ctx.upload(src,h.data(),h.size()*2); }
            else ctx.upload(src,v.data(),v.size()*4);
            ctx.transition(src,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); ctx.transition(ref,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit area{}; area.srcSubresource=area.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            area.srcOffsets[1]={int(c.vw),int(c.vh),1}; area.dstOffsets[1]={int(c.dw),int(c.dh),1};
            ctx.one_shot([&](VkCommandBuffer cmd){ vkCmdBlitImage(cmd,src.handle,src.layout,ref.handle,ref.layout,1,&area,VK_FILTER_LINEAR); });
            ctx.transition(src,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); ctx.transition(got,VK_IMAGE_LAYOUT_GENERAL);
            r.kernel.create(ctx,(shaders/module).string(),{},16,{&src,&got});
            const std::array<uint32_t,4> push{c.dw,c.dh,c.vw,c.vh}; r.Run(push,c.dw,c.dh);
            ctx.transition(ref,VK_IMAGE_LAYOUT_GENERAL);
            const size_t n=size_t(256)*160*ch; std::vector<float> a(n), b(n);
            if (half) { std::vector<uint16_t> ha(n), hb(n); ctx.download(got,ha.data(),n*2); ctx.download(ref,hb.data(),n*2);
                        for (size_t i=0;i<n;++i){a[i]=HalfFloat(ha[i]);b[i]=HalfFloat(hb[i]);} }
            else { ctx.download(got,a.data(),n*4); ctx.download(ref,b.data(),n*4); }
            float worst=0;
            for (uint32_t y=0;y<c.dh;++y) for (uint32_t x=0;x<c.dw;++x) for (uint32_t k=0;k<ch;++k) {
                const size_t i=(size_t(y)*256+x)*ch+k;
                worst=std::max(worst,std::fabs(a[i]-b[i])/std::max(1.f,std::fabs(b[i])));
            }
            std::cout<<"motion rescale "<<fm.name<<" "<<c.name<<": max rel diff vs blit="<<worst<<'\n';
            // Both filter with the hardware's fixed-point weights; allow their rounding and the half step.
            Require(worst <= (half ? 2e-2f : 1e-2f), "compute motion rescale differs from the LINEAR blit");
        }
    }
    std::cout<<"PASS motion rescale on compute matches the LINEAR blit (smaller, larger, padded)\n";
}

int main(int argc, char** argv)
{
    nrvk::Context ctx;
    try
    {
        std::cout << std::unitbuf;
        const bool negativeOnly = argc == 3 && std::string(argv[2]) == "--negative-hdr-only";
        Require(argc == 2 || negativeOnly || (argc == 3 && std::string(argv[2]) == "--fp16-only"),
                "usage: hdr_shader <shaders/runtime> [--fp16-only|--negative-hdr-only]");
        ctx.create();
        std::cout << "GPU: " << ctx.gpu_name << '\n';
        if (negativeOnly) {
            NegativeHdrReference(ctx, argv[1]);
            ctx.destroy();
            return 0;
        }
        FloatConversions(ctx, argv[1]);
        R11G11B10Unpack(ctx, argv[1]);
        R11G11B10Pack(ctx, argv[1]);
        PackedUnorm(ctx, argv[1]);
        MotionRescale(ctx, argv[1]);
        if (argc == 2) {
            Encode(ctx, argv[1]);
            Transfer(ctx, argv[1]);
            NegativeHdrReference(ctx, argv[1]);
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
