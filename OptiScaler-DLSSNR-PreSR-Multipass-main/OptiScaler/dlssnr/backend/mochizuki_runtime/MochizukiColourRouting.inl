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

    bool SharedColourEligible(const Job& j) const
    {
        if (!sharedInputRequest || j.inlineExecution || !j.motion || !runtime || runtime->reno ||
            j.canonicalise || geometry.canonicalise || geometry.bucketed || geometry.firstPass || j.firstPass ||
            j.controls.passes != 1 || net.scale != 1.f || j.motionPath != Job::Uploaded ||
            j.width != geometry.width || j.height != geometry.height ||
            j.width != geometry.colourWidth || j.height != geometry.colourHeight ||
            colourVk != VK_FORMAT_R16G16B16A16_SFLOAT)
            return false;
        const auto desc = j.colour->GetDesc();
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && desc.MipLevels == 1 &&
               desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1;
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
            nr::logf("[mochizuki] split colour input: %s generation=%llu allocation=%llu bytes family=%u frame_estimate=%llu",
                colourInput->Shared() ? "shared texture" : "buffer",
                static_cast<unsigned long long>(colourInput->generation),
                static_cast<unsigned long long>(colourInput->bytes), colourInput->family,
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
        if (colourInput && colourInput->Shared()) return;
        if (!pendingColour) {
            if (auto cached = sharedColourCache.lock(); cached &&
                cached->Matches(j.width, j.height, colourVk, kImageUsage) && cached->family == vk.family) {
                pendingColour = std::move(cached);
            }
        }
        if (pendingColour && (!pendingColour->Matches(j.width, j.height, colourVk, kImageUsage) ||
                              pendingColour->family != vk.family))
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
        try {
            auto made = std::make_shared<ImageInput>(vk.device);
            const auto desc = made->PlanShared(device, vk, j.width, j.height);
            std::string why;
            // Incremental allocation: current usage already includes the buffer
            // route and every retained recording. Do not release them speculatively.
            if (!VramAdmits(made->bytes + kVramMarginBytes, j.width, j.height, "shared colour candidate", why)) {
                sharedColourHold.Refused(geometry, why.c_str());
                nr::logf("[mochizuki] shared colour held; buffer NR retained: %s", why.c_str());
                return;
            }
            if (hooks.rejectSharedInput)
                throw NrError(NrError::Invalid, "MZ_TEST_REJECT_SHARED_INPUT: shared colour handle rejected", VK_SUCCESS, E_INVALIDARG);
            made->Import(device, vk, desc);
            // Reserve the strong initialization owner before submission can happen.
            inputInitializations.push_back(made);
            gpuUsed.store(true, std::memory_order_relaxed);
            made->Initialize(vk);
            pendingColour = std::move(made);
            sharedColourCache = pendingColour;
        } catch (const std::exception& e) {
            const auto kind = KindOf(e);
            if (kind == NrError::DeviceLost) throw;
            const auto delay = sharedColourHold.Failed(geometry, kind, e.what(), true);
            nr::logf("[mochizuki] shared colour refused; buffer NR retained (retry=%llu ms): %s",
                     static_cast<unsigned long long>(delay), e.what());
        }
    }
