// Private adapter resource. A recording retains this owner independently of the
// rest of FrameBuffers, so changing transport never changes a closed recording.
struct ColourInput
{
    VkDevice device {};
    SharedBuffer upload;
    Image image;
    ID3D12Resource* texture {};
    VkCommandPool initPool {};
    VkFence initDone {};
    bool initSubmitted = false;
    UINT width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    uint32_t family = VK_QUEUE_FAMILY_IGNORED;
    UINT64 bytes = 0;
    uint64_t finished = 0; // last accepted Vulkan submission that can read/write this carrier
    uint64_t generation = ++nextGeneration;
    static inline std::atomic<uint64_t> nextGeneration { 0 };

    explicit ColourInput(VkDevice d) : device(d) {}
    ColourInput(const ColourInput&) = delete;
    ColourInput& operator=(const ColourInput&) = delete;
    bool Shared() const { return texture != nullptr; }
    bool Matches(UINT w, UINT h, VkFormat f, VkImageUsageFlags u) const
    { return width == w && height == h && format == f && usage == u; }
    static void ImportCheck(VkResult result, const char* what)
    {
        if (result == VK_ERROR_INVALID_EXTERNAL_HANDLE) {
            char text[192];
            std::snprintf(text, sizeof text, "%s failed, VkResult %d", what, int(result));
            throw NrError(NrError::Invalid, text, result);
        }
        VkCheck(result, what);
    }

    // The session retains every pending initialization until its fence is proven
    // complete, and quarantines these owners on uncertain/device-lost completion.
    ~ColourInput()
    {
        if (initDone) vkDestroyFence(device, initDone, nullptr);
        if (initPool) vkDestroyCommandPool(device, initPool, nullptr);
        image.Release(device);
        upload.Release(device);
        if (texture) texture->Release();
    }

    void CreateBuffer(ID3D12Device* d, Vulkan& vk, const Footprint& fp, VkFormat f,
                      UINT w, UINT h, VkImageUsageFlags u)
    {
        width = w; height = h; format = f; usage = u;
        family = vk.computeQueue ? VK_QUEUE_FAMILY_IGNORED : vk.family;
        upload.Create(d, vk, fp.bytes);
        image.Create(vk, f, w, h, u);
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(vk.device, image.image, &req);
        bytes = upload.resource->GetDesc().Width + req.size;
    }

    // Query allocation sizes before allocating/importing the optional memory.
    // VkImage itself is only an unbound descriptor at this point.
    D3D12_RESOURCE_DESC PlanShared(ID3D12Device* d, Vulkan& vk, UINT w, UINT h)
    {
        width = w; height = h; format = VK_FORMAT_R16G16B16A16_SFLOAT;
        usage = kImageUsage; family = vk.family;
        const auto handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkPhysicalDeviceExternalImageFormatInfo ext { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
        ext.handleType = handleType;
        VkPhysicalDeviceImageFormatInfo2 fi { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext };
        fi.format = format; fi.type = VK_IMAGE_TYPE_2D; fi.tiling = VK_IMAGE_TILING_OPTIMAL; fi.usage = usage;
        VkExternalImageFormatProperties ep { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkImageFormatProperties2 fp { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep };
        const auto result = vkGetPhysicalDeviceImageFormatProperties2(vk.physical, &fi, &fp);
        if (result != VK_SUCCESS && result != VK_ERROR_FORMAT_NOT_SUPPORTED)
            VkCheck(result, "shared colour capabilities");
        if (result == VK_ERROR_FORMAT_NOT_SUPPORTED ||
            !(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            !(ep.externalMemoryProperties.compatibleHandleTypes & handleType) ||
            w > fp.imageFormatProperties.maxExtent.width || h > fp.imageFormatProperties.maxExtent.height)
            throw NrError(NrError::Unsupported, "shared RGBA16F colour import is unavailable");

        D3D12_FEATURE_DATA_FORMAT_SUPPORT support { DXGI_FORMAT_R16G16B16A16_FLOAT };
        Check(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support), "shared colour format");
        if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW))
            throw NrError(NrError::Unsupported, "shared RGBA16F colour texture is unavailable");
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = rd.MipLevels = rd.SampleDesc.Count = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const auto allocation = d->GetResourceAllocationInfo(0, 1, &rd);
        if (allocation.SizeInBytes == UINT64_MAX)
            throw NrError(NrError::Unsupported, "shared colour allocation description is unsupported");
        VkExternalMemoryImageCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        external.handleTypes = handleType;
        VkImageCreateInfo ci { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external };
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = { w, h, 1 };
        ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ImportCheck(vkCreateImage(device, &ci, nullptr, &image.image), "shared colour VkImage");
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(device, image.image, &req);
        bytes = std::max<UINT64>(req.size, allocation.SizeInBytes);
        return rd;
    }

    void Import(ID3D12Device* d, Vulkan& vk, const D3D12_RESOURCE_DESC& rd)
    {
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Check(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON,
                                        nullptr, IID_PPV_ARGS(&texture)), "shared colour texture");
        HANDLE handle {};
        Check(d->CreateSharedHandle(texture, nullptr, GENERIC_ALL, nullptr, &handle), "shared colour handle");
        struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close { handle };
        VkMemoryRequirements req {}; vkGetImageMemoryRequirements(device, image.image, &req);
        VkMemoryWin32HandlePropertiesKHR hp { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        ImportCheck(vk.memoryHandleProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &hp),
                    "shared colour handle properties");
        VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated.image = image.image;
        VkImportMemoryWin32HandleInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated };
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT; import.handle = handle;
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
        ai.allocationSize = bytes;
        ai.memoryTypeIndex = vk.MemoryType(req.memoryTypeBits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ImportCheck(vkAllocateMemory(device, &ai, nullptr, &image.memory), "shared colour import");
        VkCheck(vkBindImageMemory(device, image.image, image.memory, 0), "shared colour bind");
    }

    void Ownership(VkCommandBuffer c, bool acquire, VkImageLayout before, VkImageLayout after) const
    {
        VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.image = image.image; b.oldLayout = before; b.newLayout = after;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : family;
        b.dstQueueFamilyIndex = acquire ? family : VK_QUEUE_FAMILY_EXTERNAL;
        b.srcAccessMask = acquire ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = acquire ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // Called under submitMutex, before any D3D12 capture can bind this owner.
    void Initialize(Vulkan& vk)
    {
        VkCommandPoolCreateInfo pi { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pi.queueFamilyIndex = family; pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkCheck(vkCreateCommandPool(device, &pi, nullptr, &initPool), "shared colour init pool");
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = initPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkCommandBuffer c {};
        VkCheck(vkAllocateCommandBuffers(device, &ai, &c), "shared colour init command");
        VkCommandBufferBeginInfo bi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkBeginCommandBuffer(c, &bi), "shared colour init begin");
        ImageBarrier(c, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        Ownership(c, false, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        VkCheck(vkEndCommandBuffer(c), "shared colour init end");
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkCheck(vkCreateFence(device, &fi, nullptr, &initDone), "shared colour init fence");
        VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &c;
        VkCheck(vkQueueSubmit(vk.queue, 1, &si, initDone), "shared colour initialization");
        initSubmitted = true;
    }

    bool Ready() const
    {
        if (!initSubmitted) return false;
        const auto result = vkGetFenceStatus(device, initDone);
        if (result == VK_NOT_READY) return false;
        VkCheck(result, "shared colour initialization status");
        return true;
    }

    void Capture(ID3D12GraphicsCommandList* c, ID3D12Resource* source, D3D12_RESOURCE_STATES state) const
    {
        Transition(c, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(c, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        c->CopyResource(texture, source);
        Transition(c, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Transition(c, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }
};
