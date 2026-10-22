    // Included inside Session. Route selection happens before the recording's
    // FrameBuffers snapshot; BoundRecording subsequently restores its exact owner.
    void PollInputInitializations()
    {
        inputInitializations.erase(std::remove_if(inputInitializations.begin(), inputInitializations.end(),
            [](const auto& input) { return !input->initSubmitted || input->Ready(); }), inputInitializations.end());
    }

    bool InputInitializationsDone(uint32_t ms)
    {
        const auto until = GetTickCount64() + ms;
        for (const auto& input : inputInitializations)
            if (input->initSubmitted) {
                const auto now = GetTickCount64();
                const uint64_t remaining = now < until ? until - now : 0;
                if (vkWaitForFences(vk.device, 1, &input->initDone, VK_TRUE, remaining * 1'000'000) != VK_SUCCESS)
                    return false;
            }
        return true;
    }

    // The carrier is the network's extent (geometry.width x height, the bucket with DRS). Still on buffer: Plan A
    // (its convert/unconvert pass binds the buffer carrier's image at MakeBuffers and stores into it), first-pass
    // export (downloaded from a buffer, including passthrough), Inline and frames without motion.
    bool SharedColourEligible(const Job& j) const
    {
        if (!sharedInputRequest || j.inlineExecution || !j.motion || !runtime ||
            j.canonicalise || geometry.canonicalise || geometry.firstPass || j.firstPass)
            return false;
        if (!sharedExtendedRequest &&
            (runtime->reno || geometry.bucketed || j.controls.passes != 1 || net.scale != 1.f ||
             j.motionPath != Job::Uploaded || j.width != geometry.width || j.height != geometry.height ||
             j.width != geometry.colourWidth || j.height != geometry.colourHeight))
            return false;
        if (!j.width || !j.height || j.width > geometry.width || j.height > geometry.height)
            return false;
        const auto desc = j.colour->GetDesc();
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               ColourFormat(desc.Format).shared != DXGI_FORMAT_UNKNOWN &&
               ColourFormat(desc.Format).vk == colourVk && desc.MipLevels == 1 &&
               desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1 &&
               desc.Width >= j.width && desc.Height >= j.height;
    }

    void InstallColourInput(std::shared_ptr<ImageInput> next)
    {
        if (colourInput == next) return;
        if (colourInput) {
            // Keep the outgoing carrier until Vulkan itself confirms completion.
            // Closed recordings independently keep its D3D12 future uses alive.
            retiredInputs.push_back(colourInput);
            if (colourInput->Shared()) sharedColourCache = colourInput;
            else bufferColourCache = colourInput;
        }
        input = next->upload;
        colourImage = next->image;
        colourInput = std::move(next);
        std::fill(std::begin(fallbackCurrent), std::end(fallbackCurrent), false);
    }

    void LogColourInput()
    {
        if (!colourInput) return;
        if (loggedColourGeneration != colourInput->generation) {
            loggedColourGeneration = colourInput->generation;
            nr::logf("[mochizuki] split colour input: %s generation=%llu allocation=%llu bytes format=%d dxgi=%u family=%u usage=0x%x frame_estimate=%llu",
                colourInput->Shared() ? "shared texture" : "buffer",
                static_cast<unsigned long long>(colourInput->generation),
                static_cast<unsigned long long>(colourInput->bytes), int(colourInput->format),
                unsigned(colourInput->sharedFormat), colourInput->family, unsigned(colourInput->usage),
                static_cast<unsigned long long>(FrameBytes(geometry, colourInput.get(), motionInput.get())));
        }
    }

    void PollRetiredInputs()
    {
        if (retiredInputs.empty() || deviceLost) return;
        Check(device->GetDeviceRemovedReason(), "input retirement device status");
        uint64_t finished = 0;
        VkCheck(vkGetSemaphoreCounterValue(vk.device, fromVk.semaphore, &finished), "input retirement completion");
        if (finished == UINT64_MAX)
            throw NrError(NrError::DeviceLost, "input retirement has no trustworthy Vulkan completion");
        retiredInputs.erase(std::remove_if(retiredInputs.begin(), retiredInputs.end(),
            [finished](const auto& input) { return input->finished <= finished; }), retiredInputs.end());
    }

    // Switching away from a ready texture may reuse a buffer carrier still held
    // by a retained recording. Weak caches never permanently keep both carriers.
    void SelectBufferColour()
    {
        if (colourInput && !colourInput->Shared()) return;
        const auto usage = geometry.canonicalise ? kImageUsage | VK_IMAGE_USAGE_STORAGE_BIT : kImageUsage;
        auto buffer = bufferColourCache.lock();
        if (!buffer || !buffer->Matches(geometry.width, geometry.height, colourVk, usage)) {
            if (bufferColourHold.Holds(geometry, GetTickCount64()))
                throw NrError(NrError::OutOfMemory, bufferColourHold.message);
            auto made = std::make_shared<ImageInput>(vk.device);
            std::string why;
            // The regular FrameBytes estimate assumes this buffer carrier. Check
            // the incremental switch peak without invoking ReleaseNetwork.
            const UINT64 bufferBytes = std::max((colourFootprint.bytes + 65535) & ~UINT64(65535), UINT64(16) << 20);
            const UINT64 imageBytes = UINT64(geometry.width) * geometry.height * ColourFormat(
                geometry.gameColourFormat ? geometry.gameColourFormat : geometry.colourFormat).bytes + 65536;
            if (!VramAdmits(bufferBytes + imageBytes + kVramMarginBytes, geometry.width, geometry.height,
                            "the buffer colour route", why)) {
                bufferColourHold.Refused(geometry, why.c_str());
                throw NrError(NrError::OutOfMemory, why);
            }
            try {
                made->CreateBuffer(device, vk, colourFootprint, colourVk, geometry.width, geometry.height, usage);
            } catch (const std::exception& e) {
                const auto kind = KindOf(e);
                if (kind != NrError::DeviceLost) bufferColourHold.Failed(geometry, kind, e.what(), true);
                throw;
            }
            bufferColourHold = {};
            buffer = std::move(made);
        }
        InstallColourInput(std::move(buffer));
    }

    void SelectColourInput(const Job& j)
    {
        PollInputInitializations();
        PollRetiredInputs();
        if (!SharedColourEligible(j)) {
            pendingColour.reset(); // pending submissions are still retained by inputInitializations
            SelectBufferColour();
            return;
        }
        const auto typed = ColourFormat(j.colour->GetDesc().Format).shared;
        const auto expectedUsage = [&] {
            return kImageUsage | (colourVk == VK_FORMAT_R16G16B16A16_SFLOAT && !sharedColourStorageRefused ?
                VK_IMAGE_USAGE_STORAGE_BIT : 0);
        };
        if (colourInput && colourInput->Shared()) return;
        if (!pendingColour) {
            if (auto cached = sharedColourCache.lock(); cached &&
                cached->Matches(geometry.width, geometry.height, colourVk, expectedUsage()) && cached->sharedFormat == typed && cached->family == vk.family) {
                pendingColour = std::move(cached);
            }
        }
        if (pendingColour && (!pendingColour->Matches(geometry.width, geometry.height, colourVk, expectedUsage()) ||
                              pendingColour->sharedFormat != typed || pendingColour->family != vk.family))
            pendingColour.reset();
        if (pendingColour) {
            if (pendingColour->Ready() && !sharedInputDelay) {
                InstallColourInput(std::move(pendingColour));
                sharedColourHold = {};
            } else if (sharedInputDelay) --sharedInputDelay;
            return;
        }
        if (sharedColourHold.Holds(geometry, GetTickCount64())) return;
        // An optional route must not stall the render thread behind a network
        // build on a device with no separate build queue. Try again next frame.
        std::unique_lock lock(submitMutex, std::try_to_lock);
        if (!lock) return;
        bool budgetRefused = false;
        try {
            auto create = [&](bool storage) {
                auto made = std::make_shared<ImageInput>(vk.device);
                const auto desc = made->PlanShared(device, vk, geometry.width, geometry.height, colourVk, typed, storage);
                std::string why;
                // Include actual allocation size and all still-retained carriers.
                budgetRefused = !VramAdmits(made->bytes + kVramMarginBytes, geometry.width, geometry.height,
                                            "shared colour candidate", why);
                if (budgetRefused) throw NrError(NrError::OutOfMemory, why.c_str());
                if (hooks.rejectSharedInput || (storage && hooks.rejectSharedStorage))
                    throw NrError(NrError::Invalid, "shared colour handle rejected (test hook)", VK_SUCCESS, E_INVALIDARG);
                made->Import(device, vk, desc);
                return made;
            };
            std::shared_ptr<ImageInput> made;
            if (expectedUsage() & VK_IMAGE_USAGE_STORAGE_BIT) {
                try { made = create(true); }
                catch (const std::exception& e) {
                    if (KindOf(e) == NrError::DeviceLost) throw;
                    // Failed/unsupported optional capabilities never remove the
                    // sampled route. Latch once, avoiding allocation churn.
                    sharedColourStorageRefused = true;
                    nr::logf("[mochizuki] shared colour storage preference unavailable; sampled texture retained: %s", e.what());
                }
            }
            if (!made) made = create(false);
            // Reserve the strong initialization owner before submission can happen.
            inputInitializations.push_back(made);
            gpuUsed.store(true, std::memory_order_relaxed);
            made->Initialize(vk);
            pendingColour = std::move(made);
            sharedColourCache = pendingColour;
        } catch (const std::exception& e) {
            const auto kind = KindOf(e);
            if (kind == NrError::DeviceLost) throw;
            if (budgetRefused) {
                sharedColourHold.Refused(geometry, e.what());
                nr::logf("[mochizuki] shared colour held; buffer NR retained: %s", e.what());
                return;
            }
            const auto delay = sharedColourHold.Failed(geometry, kind, e.what(), true);
            nr::logf("[mochizuki] shared colour refused; buffer NR retained (retry=%llu ms): %s",
                     static_cast<unsigned long long>(delay), e.what());
        }
    }
