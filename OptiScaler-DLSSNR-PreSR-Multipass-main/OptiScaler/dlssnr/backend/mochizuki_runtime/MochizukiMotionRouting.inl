    // Included inside Session. Motion admission is independent of colour's
    // optional allocation: failure leaves shared-colour + buffer-motion NR live.
    bool SharedMotionEligible(const Job& j) const
    {
        if (!sharedMotionRequest || !colourInput || !colourInput->Shared() ||
            !SharedColourEligible(j) || geometry.computeMotion ||
            j.motionWidth != j.width || j.motionHeight != j.height ||
            j.motionWidth != geometry.motionWidth || j.motionHeight != geometry.motionHeight ||
            (motionVk != VK_FORMAT_R16G16_SFLOAT && motionVk != VK_FORMAT_R16G16B16A16_SFLOAT))
            return false;
        const auto desc = j.motion->GetDesc();
        const auto dxgi = motionVk == VK_FORMAT_R16G16_SFLOAT ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == dxgi &&
               desc.Width == j.motionWidth && desc.Height == j.motionHeight &&
               desc.MipLevels == 1 && desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1;
    }

    void InstallMotionInput(std::shared_ptr<ImageInput> next)
    {
        if (motionInput == next) return;
        if (motionInput) {
            // Shared with colour's retirement proof; recordings also retain
            // future D3D12 uses even after the last Vulkan value has completed.
            retiredInputs.push_back(motionInput);
            if (motionInput->Shared()) sharedMotionCache = motionInput;
            else bufferMotionCache = motionInput;
        }
        motionBuffer = next->upload;
        motionImage = next->image;
        motionInput = std::move(next);
    }

    void LogMotionInput()
    {
        if (!motionInput || loggedMotionGeneration == motionInput->generation) return;
        loggedMotionGeneration = motionInput->generation;
        nr::logf("[mochizuki] split motion input: %s generation=%llu allocation=%llu bytes format=%d family=%u frame_estimate=%llu",
            motionInput->Shared() ? "shared texture" : "buffer",
            static_cast<unsigned long long>(motionInput->generation),
            static_cast<unsigned long long>(motionInput->bytes), int(motionInput->format), motionInput->family,
            static_cast<unsigned long long>(FrameBytes(geometry, colourInput.get(), motionInput.get())));
    }

    void SelectBufferMotion()
    {
        if (!motionInput || !motionInput->Shared()) return;
        auto buffer = bufferMotionCache.lock();
        if (!buffer || !buffer->Matches(geometry.motionWidth, geometry.motionHeight, motionVk, kImageUsage)) {
            if (bufferMotionHold.Holds(geometry, GetTickCount64()))
                throw NrError(NrError::OutOfMemory, bufferMotionHold.message);
            auto made = std::make_shared<ImageInput>(vk.device);
            const UINT64 bufferBytes = std::max((motionFootprint.bytes + 65535) & ~UINT64(65535), UINT64(16) << 20);
            const UINT64 imageBytes = UINT64(geometry.motionWidth) * geometry.motionHeight * MotionFormat(geometry.motionFormat).bytes + 65536;
            std::string why;
            if (!VramAdmits(bufferBytes + imageBytes + kVramMarginBytes, geometry.width, geometry.height,
                            "the buffer motion route", why)) {
                bufferMotionHold.Refused(geometry, why.c_str());
                throw NrError(NrError::OutOfMemory, why);
            }
            try {
                made->CreateBuffer(device, vk, motionFootprint, motionVk, geometry.motionWidth, geometry.motionHeight, kImageUsage);
            } catch (const std::exception& e) {
                const auto kind = KindOf(e);
                if (kind != NrError::DeviceLost) bufferMotionHold.Failed(geometry, kind, e.what(), true);
                throw;
            }
            bufferMotionHold = {};
            buffer = std::move(made);
        }
        InstallMotionInput(std::move(buffer));
    }

    void SelectMotionInput(const Job& j)
    {
        // SelectColourInput has already polled the common initialization and
        // retirement lists. No game command can bind a not-ready input.
        if (!SharedMotionEligible(j)) {
            pendingMotion.reset(); // any submitted init stays in inputInitializations
            SelectBufferMotion();
            return;
        }
        if (motionInput && motionInput->Shared()) return;
        if (!pendingMotion) {
            if (auto cached = sharedMotionCache.lock(); cached &&
                cached->Matches(j.motionWidth, j.motionHeight, motionVk, kImageUsage) && cached->family == vk.family)
                pendingMotion = std::move(cached);
        }
        if (pendingMotion && (!pendingMotion->Matches(j.motionWidth, j.motionHeight, motionVk, kImageUsage) ||
                              pendingMotion->family != vk.family))
            pendingMotion.reset();
        if (pendingMotion) {
            if (pendingMotion->Ready() && !sharedMotionDelay) {
                InstallMotionInput(std::move(pendingMotion));
                sharedMotionHold = {};
            } else if (sharedMotionDelay) --sharedMotionDelay;
            return;
        }
        if (sharedMotionHold.Holds(geometry, GetTickCount64())) return;
        std::unique_lock lock(submitMutex, std::try_to_lock);
        if (!lock) return;
        try {
            auto made = std::make_shared<ImageInput>(vk.device);
            const auto desc = made->PlanShared(device, vk, j.motionWidth, j.motionHeight, motionVk);
            std::string why;
            if (!VramAdmits(made->bytes + kVramMarginBytes, geometry.width, geometry.height, "shared motion candidate", why)) {
                sharedMotionHold.Refused(geometry, why.c_str());
                nr::logf("[mochizuki] shared motion held; colour route and buffer motion NR retained: %s", why.c_str());
                return;
            }
            if (hooks.rejectSharedMotion)
                throw NrError(NrError::Invalid, "MZ_TEST_REJECT_SHARED_MOTION: shared motion handle rejected", VK_SUCCESS, E_INVALIDARG);
            made->Import(device, vk, desc);
            inputInitializations.push_back(made); // register strong owner before submission
            gpuUsed.store(true, std::memory_order_relaxed);
            made->Initialize(vk);
            pendingMotion = std::move(made);
            sharedMotionCache = pendingMotion;
        } catch (const std::exception& e) {
            const auto kind = KindOf(e);
            if (kind == NrError::DeviceLost) throw;
            const auto delay = sharedMotionHold.Failed(geometry, kind, e.what(), true);
            nr::logf("[mochizuki] shared motion refused; colour route and buffer motion NR retained (retry=%llu ms): %s",
                     static_cast<unsigned long long>(delay), e.what());
        }
    }
