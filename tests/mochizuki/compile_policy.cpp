// No GPU or model weights: use the production background-policy scope, core
// compiler TLS and bundle validators, including exceptions on a real worker.
#include "dlssnr/backend/mochizuki_runtime/MochizukiCompilePolicy.h"
#include <nr_pipeline_binary.hpp>
#include <nr_shader_manifest.hpp>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using mz::compile::CompilePolicy;

// Link only the core dispatch boundary, not a Vulkan loader/device. Any PAL
// import/template work in compile-only mode reaches an explicitly failing stub.
static thread_local VkResult driverResult = VK_SUCCESS;
static thread_local unsigned driverCalls = 0, importCalls = 0;
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t count,
    const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* pipelines)
{
    ++driverCalls;
    if (driverResult == VK_SUCCESS && count == 1)
        *pipelines = reinterpret_cast<VkPipeline>(uintptr_t(17));
    return driverResult;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*)
{
    ++importCalls;
    return nullptr;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*,
    const VkAllocationCallbacks*, VkShaderModule*)
{
    ++importCalls;
    return VK_ERROR_INITIALIZATION_FAILED;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*)
{
    ++importCalls;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*)
{
    ++importCalls;
}
}

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

template<class F>
static void rejects(F&& action, const char* message)
{
    try { action(); }
    catch (const std::exception& error)
    {
        require(std::string(error.what()).find(message) != std::string::npos,
                "failure lost its actionable diagnostic");
        return;
    }
    throw std::runtime_error("invalid policy operation was accepted");
}

static VkResult compile_network(const std::string& path)
{
    VkComputePipelineCreateInfo info {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.pName = "main";
    VkPipeline pipeline {};
    // No record, shell module, cache or enabled-device mark exists. The real
    // compile-only branch must go straight to vkCreateComputePipelines.
    return nr::binary::create_or_default(reinterpret_cast<VkDevice>(uintptr_t(1)), VK_NULL_HANDLE,
        info, path, nullptr, 0, &pipeline, 0);
}

static void put(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    require(bool(output), "cannot write test fixture");
}

static const std::map<std::string, uint32_t> constants {
    {"vattn_qt", 32}, {"gemm_wide_mt", 64}, {"gemm_wide_nt", 256}
};
static const char* manifest =
    "nr_shader_manifest v3\nweight_layout 3\nmath_profile 3\n"
    "vattn_qt 32\ngemm_wide_mt 64\ngemm_wide_nt 256\n";

static std::string spirv_header(uint32_t magic = 0x07230203, uint32_t version = 0x00010300,
                                uint32_t bound = 1, uint32_t schema = 0)
{
    const uint32_t words[] {magic, version, 0, bound, schema};
    return {reinterpret_cast<const char*>(words), sizeof(words)};
}

static void bundle_validation(const fs::path& scratch)
{
    const auto shaders = scratch / "linux" / "shaders";
    rejects([&] { nr::detail::require_linux_shader_bundle(shaders, "fp32", constants); }, "missing directory");
    fs::create_directories(shaders);
    rejects([&] { nr::detail::require_linux_shader_bundle(shaders, "fp32", constants); }, "accumulation marker");
    for (const char* marker : {"round-fp16\n", "fp32 extra\n", "\n"})
    {
        put(shaders / "accumulation.txt", marker);
        rejects([&] { nr::detail::require_linux_shader_bundle(shaders, "fp32", constants); }, "accumulation marker");
    }
    put(shaders / "accumulation.txt", "fp32\n");
    rejects([&] { nr::detail::require_linux_shader_bundle(shaders, "fp32", constants); }, "missing shader-constants.txt");
    put(shaders / "shader-constants.txt", manifest);
    const auto values = nr::detail::require_linux_shader_bundle(shaders, "fp32", constants);
    require(values.at("weight_layout") == 3 && values.at("math_profile") == 3,
            "Linux bundle profile was not read");
    auto wrong = constants;
    wrong.at("gemm_wide_nt") = 128;
    rejects([&] { nr::detail::require_linux_shader_bundle(shaders, "fp32", wrong); }, "constant mismatch: gemm_wide_nt");
    put(shaders / "shader-constants.txt", "nr_shader_manifest v3\nvattn_qt 32\ngemm_wide_mt 64\ngemm_wide_nt 256\n");
    rejects([&] { nr::detail::require_linux_shader_bundle(shaders, "fp32", constants); }, "weight_layout");
    put(shaders / "shader-constants.txt", manifest);

    const std::vector<std::string> graph {"g_selected.spv", "g_fswinimagepost32_grade.spv"};
    const std::vector<std::string> temporal {"temporal_pre_fp32.spv", "temporal_post_fp32.spv"};
    rejects([&] { nr::detail::require_shader_assets(shaders, graph); }, "missing required shader");
    // An unrelated shader, including a Windows copy, cannot satisfy a missing
    // selected Linux kernel. No validator is allowed to search another root.
    put(scratch / "windows" / "shaders" / "g_selected.spv", spirv_header());
    put(shaders / "g_unselected.spv", spirv_header());
    rejects([&] { nr::detail::require_shader_assets(shaders, graph); }, "g_selected.spv");
    put(shaders / "g_selected.spv", "");
    rejects([&] { nr::detail::require_shader_assets(shaders, graph); }, "g_selected.spv");
    for (const auto& file : graph) put(shaders / file, spirv_header());
    fs::remove(shaders / "g_fswinimagepost32_grade.spv");
    rejects([&] { nr::detail::require_shader_assets(shaders, graph); }, "g_fswinimagepost32_grade.spv");
    put(shaders / "g_fswinimagepost32_grade.spv", spirv_header());
    nr::detail::require_shader_assets(shaders, graph);
    rejects([&] { nr::detail::require_shader_assets(shaders / "temporal", temporal); }, "missing required shader");
    for (const auto& file : temporal) put(shaders / "temporal" / file, spirv_header());
    put(shaders / "temporal" / "temporal_post_fp32.spv", "");
    rejects([&] { nr::detail::require_shader_assets(shaders / "temporal", temporal); }, "temporal_post_fp32.spv");
    put(shaders / "temporal" / "temporal_post_fp32.spv", spirv_header());
    nr::detail::require_shader_assets(shaders / "temporal", temporal);
    for (const auto& invalid : {spirv_header(0), spirv_header(0x07230203, 0x00010700),
                               spirv_header(0x07230203, 0x00010301), spirv_header(0x07230203, 0x00010300, 0),
                               spirv_header(0x07230203, 0x00010300, 1, 1), spirv_header() + "x",
                               std::string("junk"), spirv_header().substr(0, 16)})
    {
        put(shaders / "g_selected.spv", invalid);
        rejects([&] { nr::detail::require_shader_assets(shaders, graph); }, "invalid required shader");
    }
    put(shaders / "g_selected.spv", spirv_header());
}

static void background_builds(const fs::path& scratch)
{
    static_assert(uint32_t(VK_DRIVER_ID_MESA_RADV) == 3);
    require(mz::compile::PolicyForDriver(VK_DRIVER_ID_MESA_RADV) == CompilePolicy::LinuxSpirvDriverCompile,
            "RADV did not select mandatory Linux SPIR-V");
    for (const auto driver : {0u, 1u, 2u, 4u, 5u, 6u, 10u, 0x7fffffffu})
        require(mz::compile::PolicyForDriver(driver) == CompilePolicy::WindowsDefault,
                "non-RADV driver changed compiler policy");
    mz::compile::GuardWindowsFallback(CompilePolicy::WindowsDefault);
    rejects([] { mz::compile::GuardWindowsFallback(CompilePolicy::LinuxSpirvDriverCompile); }, "fallback is forbidden");

    nr::binary::own_directory();
    nr::binary::configure("caller-windows-aco", {"caller-shell"}, "caller-cache", {{"caller", "alias"}});
    nr::binary::mode().imported = 9;
    const auto linuxBundle = (scratch / "linux").string();
    const auto policy = mz::compile::PolicyForDriver(VK_DRIVER_ID_MESA_RADV);
    std::promise<void> configured, release;
    auto configuredFuture = configured.get_future();
    auto releaseFuture = release.get_future();
    std::exception_ptr workerError;
    std::thread worker([&, policy]
    {
        bool notified = false;
        try
        {
            require(nr::binary::directory().empty(), "worker inherited caller compiler TLS");
            unsigned resets = 0;
            const auto reset = [&]() noexcept
            {
                ++resets;
                nr::binary::configure("", {}, "", {});
            };
            const auto configure = [&](CompilePolicy captured)
            {
                nr::binary::own_directory();
                require(captured == CompilePolicy::LinuxSpirvDriverCompile, "background build lost captured policy");
                nr::binary::configure(linuxBundle, {}, "", {}, false);
            };
            {
                mz::compile::BackgroundScope scope(policy, configure, reset);
                require(nr::binary::directory() == linuxBundle && !nr::binary::import_enabled(),
                        "RADV worker did not keep compile-only bundle");
                const auto windowsShaders = scratch / "windows" / "shaders";
                const auto selected = nr::detail::network_shader_directory(windowsShaders, nr::binary::directory());
                require(selected == scratch / "linux" / "shaders" && selected != windowsShaders,
                        "compile-only Linux mode selected the Windows network source");
                require(nr::detail::network_shader_directory(windowsShaders, "") == windowsShaders,
                        "ordinary Windows network source changed");
                require(nr::binary::shell_dirs().empty() && nr::binary::cache_dir().empty() &&
                        nr::binary::alias_of("caller").empty() && nr::binary::mode().imported == 0,
                        "background policy retained Windows import state");
                require(compile_network((selected / "g_selected.spv").string()) == VK_SUCCESS &&
                        driverCalls == 1 && importCalls == 0 && nr::binary::mode().imported == 0,
                        "compile-only network did not use ordinary driver compilation");
                configured.set_value(); notified = true;
                releaseFuture.wait();
            }
            require(resets == 1 && nr::binary::directory().empty(), "successful build did not clear compiler mode");

            rejects([&]
            {
                mz::compile::BackgroundScope scope(policy, configure, reset);
                require(!nr::binary::import_enabled(), "failed RADV build enabled PAL imports");
                driverResult = VK_ERROR_INITIALIZATION_FAILED;
                nr::binary::check(compile_network((scratch / "linux" / "shaders" / "g_selected.spv").string()),
                                  "forced pipeline compile failure");
            }, "forced pipeline compile failure");
            require(resets == 2 && nr::binary::directory().empty(), "compile failure leaked compiler mode");
            require(driverCalls == 2 && importCalls == 0, "failed Linux compile attempted a PAL import");
            driverResult = VK_SUCCESS;
            rejects([&]
            {
                mz::compile::BackgroundScope scope(policy, [&](CompilePolicy captured)
                {
                    configure(captured);
                    nr::detail::require_linux_shader_bundle(scratch / "absent" / "shaders", "fp32", constants);
                }, reset);
            }, "missing directory");
            require(resets == 3 && nr::binary::directory().empty(), "setup failure leaked compiler mode");

            bool windowsConstructed = false;
            rejects([&]
            {
                mz::compile::BackgroundScope scope(policy, configure, reset);
                try
                {
                    driverResult = VK_ERROR_INITIALIZATION_FAILED;
                    nr::binary::check(compile_network((scratch / "linux" / "shaders" / "g_selected.spv").string()),
                                      "forced pipeline compile failure");
                }
                catch (const std::exception&)
                {
                    mz::compile::GuardWindowsFallback(policy);
                    windowsConstructed = true;
                    nr::binary::configure("", {}, "", {});
                }
            }, "fallback is forbidden");
            require(!windowsConstructed && resets == 4 && nr::binary::directory().empty(),
                    "failed mandatory build reached Windows source");
            require(driverCalls == 3 && importCalls == 0, "mandatory compile failure retried another shader source");
            driverResult = VK_SUCCESS;

            // Reusing this exact OS thread must not retain RADV policy or its
            // bundle when the next device takes the ordinary Windows route.
            {
                mz::compile::BackgroundScope scope(CompilePolicy::WindowsDefault, [&](CompilePolicy captured)
                {
                    require(captured == CompilePolicy::WindowsDefault, "next build inherited RADV policy");
                    nr::binary::configure("next-windows-aco", {"next-shell"}, "next-cache", {{"next", "alias"}});
                }, reset);
                require(nr::binary::import_enabled() && nr::binary::directory() == "next-windows-aco",
                        "Windows build did not restore import mode");
                mz::compile::GuardWindowsFallback(CompilePolicy::WindowsDefault);
            }
            require(resets == 5 && nr::binary::directory().empty() && nr::binary::shell_dirs().empty() &&
                    nr::binary::cache_dir().empty() && nr::binary::alias_of("next").empty() &&
                    nr::binary::mode().imported == 0, "next build retained compiler state");
        }
        catch (...)
        {
            workerError = std::current_exception();
            if (!notified) configured.set_value();
        }
    });
    configuredFuture.wait();
    // Observe the caller while the other thread's Linux mode is still live.
    const bool callerIntact = nr::binary::directory() == "caller-windows-aco" && nr::binary::import_enabled() &&
                             nr::binary::mode().imported == 9 && nr::binary::cache_dir() == "caller-cache";
    release.set_value();
    worker.join();
    nr::binary::configure("", {}, "", {});
    if (workerError) std::rethrow_exception(workerError);
    require(callerIntact, "background Linux configuration mutated caller compiler TLS");
}

int main(int argc, char** argv) try
{
    require(argc == 2, "scratch parent directory required");
    // Unique per invocation; never remove an existing user-owned directory.
    const auto scratch = fs::path(argv[1]) / ("compile-policy-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    require(fs::create_directory(scratch), "cannot create isolated scratch fixture");
    struct Cleanup
    {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup {scratch};
    bundle_validation(scratch);
    background_builds(scratch);
    std::cout << "MOCHIZUKI_COMPILE_POLICY_CPU_OK\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
