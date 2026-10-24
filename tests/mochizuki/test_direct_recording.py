"""Review-v7 source contracts; not a substitute for Proton GPU acceptance."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler"
RUNTIME = SRC / "dlssnr/backend/mochizuki_runtime"


def source(path):
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(text, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*(?:(?:noexcept|override)\s*)*\{", text)
    if not match:
        raise AssertionError("Missing implementation: " + name)
    start = match.end()
    depth = 1
    for pos in range(start, len(text)):
        depth += (text[pos] == "{") - (text[pos] == "}")
        if not depth:
            return text[start:pos]
    raise AssertionError("Unterminated implementation: " + name)


class DirectRecordingContract(unittest.TestCase):
    def test_exact_frame_and_export_contract(self):
        api = source(RUNTIME / "MochizukiNrApi.h")
        self.assertRegex(api, r"MOCHIZUKI_NR_FRAME_FLAG_DIRECT_RECORDING\s+\(1u\s*<<\s*16\)")
        for name, args in (
            ("GetDirectCommandBuffer", r"void\s*\*\s*\w*\s*,\s*void\s*\*\s*\w*\s*,\s*uint64_t\s*\*\s*\w*"),
            ("RecordDirect", r"void\s*\*\s*\w*\s*,\s*void\s*\*\s*\w*\s*,\s*void\s*\*\s*\w*\s*,\s*uint64_t\s*\w*"),
        ):
            self.assertRegex(api, r"int32_t\s*\(\s*\*PFN_MochizukiNr" + name + r"\)\s*\(" + args + r"\)")
            self.assertIn("MochizukiNr" + name + "(", api)
        frame = api[api.index("typedef struct MochizukiNrFrameInfo"):api.index("} MochizukiNrFrameInfo")]
        for field in ("depth", "depth_state", "depth_width", "depth_height", "depth_inverted"):
            self.assertRegex(frame, r"\b" + field + r"\b")
        self.assertLess(frame.index("reset"), frame.index("depth"), "Depth fields append to the frame ABI")
        runtime = source(RUNTIME / "MochizukiNrRuntime.cpp")
        self.assertRegex(runtime, r"struct_size\s*!=\s*sizeof\(MochizukiNrFrameInfo\)")

    def test_pinned_command_list_extension_vtable(self):
        interop = source(RUNTIME / "MochizukiVkd3dInterop.h")
        self.assertIn('MIDL_INTERFACE("77a86b09-2bea-4801-b89a-37648e104af1")', interop)
        extension = interop[interop.index("ID3D12GraphicsCommandListExt :"):interop.index("ID3D12DXVKInteropDevice1 :")]
        self.assertRegex(extension, r"GetVulkanHandle\(VkCommandBuffer\* command\)")
        self.assertLess(extension.index("GetVulkanHandle"), extension.index("LaunchCubinShader"))

    def test_direct_runtime_never_submits_or_synthesizes_execution(self):
        direct = source(RUNTIME / "MochizukiDirectRecording.inl")
        query = body(direct, "GetDirectCommandBuffer")
        self.assertIn("GetVulkanHandle", query)
        self.assertIn("LMXXF_NR_OK", query)
        self.assertLess(query.index("*commandBuffer = 0"), query.index("if (!s->vk.adoptedVkd3d) return LMXXF_NR_OK"))
        self.assertIn("LMXXF_NR_UNAVAILABLE", query)
        self.assertLess(query.index("!s->vk.adoptedVkd3d"), query.index("GetVulkanHandle"))
        record = body(direct, "RecordDirect")
        for forbidden in ("vkQueueSubmit(", "vkBeginCommandBuffer(", "vkEndCommandBuffer(",
                          "BeginRecordingExecution(", "EndRecordingExecution(", "EnqueueHip("):
            self.assertNotIn(forbidden, record)
        self.assertIn("VK_PIPELINE_STAGE_ALL_COMMANDS_BIT", direct)
        self.assertIn("GetVulkanImageLayout", direct)
        self.assertIn("ForgetDirectPipeline", record)
        invalidate = body(direct, "ForgetDirectPipeline")
        self.assertRegex(invalidate, r"SetPipelineState\(nullptr\)")
        self.assertRegex(invalidate, r"SetComputeRootSignature\(nullptr\)")
        self.assertRegex(direct, r"ClearUnorderedAccessView(?:Uint|Float)\(")
        resource = body(direct, "DirectResource")
        self.assertIn("out.after = out.before", resource)
        self.assertIn("current != commandBuffer", record)
        for restore in (
            "imageBarrier(from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, from.after)",
            "imageBarrier(target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target.after)",
            "imageBarrier(firstTarget.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, firstTarget.after)",
        ):
            self.assertIn(restore, record)
        self.assertGreaterEqual(record.count("everything(VK_ACCESS_MEMORY_WRITE_BIT"), 2)
        invalidator = body(direct, "EnsureDirectInvalidator")
        self.assertIn("desc.Width = 2", invalidator)
        self.assertIn("desc.Height =", invalidator)
        self.assertRegex(invalidate, r"D3D12_RECT\s+rect\s*\{\s*0,\s*0,\s*1,\s*1\s*\}")
        self.assertLess(record.rindex("target.after"), record.rindex("ForgetDirectPipeline"))

    def test_native_resolution_and_direct_branch_precede_bridge(self):
        host = source(SRC / "dlssnr/backend/MochizukiBackend.cpp")
        native = host.index("logical->RecordingNativeList()")
        query = host.index("p->getDirectCommandBuffer(", native)
        self.assertLess(native, query)
        self.assertLess(query, host.index("logical->IsSplitIneligible()"))
        prepare = host.index("p->prepare(", query)
        attach = host.index("LmxxfRecording::Attach(", prepare)
        record = host.index("p->recordDirect(", attach)
        self.assertLess(prepare, attach)
        self.assertLess(attach, record)
        self.assertLess(record, host.index("p->api.RecordInputs(", record))
        direct = host[host.rfind("if (directRecording)", attach, record):host.index("p->api.RecordInputs(", record)]
        self.assertIn("InvalidateDirectComputeBindings()", direct)
        self.assertIn("return", direct)
        for forbidden in ("RecordOutputs(", "EnqueueHip(", "Split(", "ApplyTo(", "SetPipelineState(", "SetComputeRootSignature("):
            self.assertNotIn(forbidden, direct)
        self.assertIn("MOCHIZUKI_NR_FRAME_FLAG_DIRECT_RECORDING", host)
        owner = source(SRC / "dlssnr/backend/LmxxfRecordingOwner.h")
        self.assertIn("directRecording", owner)
        self.assertIn("directAccepted", owner)
        finish = body(owner, "FinishExecution")
        self.assertIn("directRecording ? e.producerSubmitted && ready : e.continuationSubmitted", finish)
        self.assertRegex(finish, r"EndRecordingExecution\(owner->context, job, e.queue,\s*flags, e.fence, e.fenceValue, e.status\)")
        between = body(owner, "Between")
        self.assertRegex(between, r"if\s*\(directRecording\s*\|\|[^)]*\)\s*return")
        proxy = source(SRC / "dlssnr/submission/CommandListProxy.h")
        invalidate = body(proxy, "InvalidateDirectComputeBindings")
        for forbidden in ("ApplyTo(", "SetPipelineState(", "SetComputeRootSignature("):
            self.assertNotIn(forbidden, invalidate)
        self.assertIn("SetDescriptorHeaps(contState.numHeaps, contState.heaps)", invalidate)
        for state in ("contState", "inlineState"):
            self.assertIn(state + ".OnPso(nullptr)", invalidate)
            self.assertIn(state + ".OnComputeRoot(nullptr)", invalidate)
            self.assertIn(state + ".computeRoots.Reset()", invalidate)
        begin_state = body(proxy, "BeginDirectState")
        for gate in ("renderPassActive", "predicationActive", "presentCommandsUsed", "resBook.CanSplit(nullptr)"):
            self.assertIn(gate, begin_state)
        self.assertNotIn("queryBook.CanSplit", begin_state)
        self.assertIn("BeginNrState(count, resources, states, false)", begin_state)
        self.assertIn("resBook.TryGet", begin_state)
        end_state = body(proxy, "EndDirectState")
        self.assertIn("TransitionEnhanced", end_state)
        self.assertIn("enhancedBook.Restore", end_state)
        for forbidden in ("ApplyTo(", "SetPipelineState(", "SetComputeRootSignature("):
            self.assertNotIn(forbidden, end_state)
        normalize = host.index("resources->BeginDirectState(", query)
        refreshed = host.index("p->getDirectCommandBuffer(", normalize)
        self.assertLess(refreshed, prepare)
        self.assertIn("~DirectStateScope() { if (resources) resources->EndDirectState(); }", host)

    def test_descriptor_scope_is_owned_until_recording_retirement(self):
        direct = source(RUNTIME / "MochizukiDirectRecording.inl")
        record = body(direct, "RecordDirect")
        self.assertIn("s->runtime->begin_recording_resources()", record)
        self.assertGreaterEqual(record.count("r->coreResources = s->runtime->end_recording_resources()"), 2)
        runtime = source(RUNTIME / "MochizukiNrRuntime.cpp")
        self.assertIn("std::shared_ptr<void> coreResources", runtime)
        self.assertIn("s->runtime->reno->begin_recording_resources()", record)
        self.assertGreaterEqual(record.count("r->renoResources = s->runtime->reno->end_recording_resources()"), 2)
        self.assertIn("std::shared_ptr<void> renoResources", runtime)
        vendor = source(ROOT / "third_party/mochizuki/windows/src/core/nr_runtime.cpp")
        for fresh in ("frames", "depth", "direct"):
            self.assertIn("recording_resources->" + fresh + ".emplace_back()", vendor)
        self.assertIn("recording_resources->timings.push_back(recording_timing)", vendor)
        self.assertIn("recording_resources->histories", vendor)
        self.assertIn("return std::move(impl_->recording_resources)", vendor)
        lifecycle = source(RUNTIME / "MochizukiRecording.inl")
        end = body(lifecycle, "EndRecordingExecution")
        self.assertNotIn("coreResources.reset", end)
        invalidate = body(lifecycle, "InvalidateRecording")
        self.assertNotIn("coreResources.reset", invalidate)
        self.assertNotIn("renoResources.reset", end)
        self.assertNotIn("renoResources.reset", invalidate)
        reno = source(ROOT / "third_party/mochizuki/windows/src/core/nr_renodx.cpp")
        for scope in ("Stage::begin_recording_resources", "Stage::end_recording_resources"):
            self.assertIn(scope, reno)
        self.assertIn("recording_resources->borrows.emplace_back()", reno)
        self.assertIn("recording_resources ? recording_resources->sets : this->sets", reno)
        self.assertIn("recording_resources ? recording_pool() : pool", reno)
        self.assertIn("recording_resources->images", reno)
        self.assertIn("return std::move(impl_->recording_resources)", reno)
        observe = body(lifecycle, "ObserveDirectCompletions")
        for gate in ("r.tail", "r.executingQueue", "r.uncertain", "r.completedExecutions == r.executions", "completed == UINT64_MAX", "completed < r.tailValue"):
            self.assertIn(gate, observe)
        self.assertLess(observe.index("completed < r.tailValue"), observe.index("r.directTiming->Harvest"))
        self.assertIn("ObserveDirectCompletions(s)", body(lifecycle, "PrepareFrame"))

    def test_real_tail_credentials_and_retirement(self):
        recording = source(RUNTIME / "MochizukiRecording.inl")
        end = body(recording, "EndRecordingExecution")
        for credential in ("fence", "value", "signalStatus", "CheckExecutionDevice"):
            self.assertIn(credential, end)
        self.assertRegex(end, r"!fence\s*\|\|\s*!value\s*\|\|\s*FAILED\(signalStatus\)")
        self.assertIn("uncertainExecution = true", end)
        self.assertIn("r->tailValue = value", end)
        collect = body(recording, "CollectRecording")
        for gate in ("invalidated", "executingQueue", "uncertain", "GetCompletedValue", "r->tailValue"):
            self.assertIn(gate, collect)
        self.assertLess(collect.index("GetCompletedValue"), collect.index("recordings.erase"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
