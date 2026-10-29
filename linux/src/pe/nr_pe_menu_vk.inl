// Included by nr_pe_menu.cpp: Vulkan drawing of the same Menu used by the DXGI backends.
// The render passes and HDR composition follow the earlier Linux Vulkan layer's; no second set of pages or input handling.
void vk_check(VkResult r, const char* op) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(op) + ": " + std::to_string(r));
}
Encoding encoding_of(VkFormat format, VkColorSpaceKHR space) {
    constexpr float kWhiteNits = 203.f;   // the menu's white in HDR (BT.2408's graphics white)
    if (space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) return {2, kWhiteNits / 80.f, "scRGB, white at 203 nits"};
    if (space == VK_COLOR_SPACE_HDR10_ST2084_EXT) return {3, kWhiteNits / 10000.f, "HDR10 (PQ), white at 203 nits"};
    switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return {1, 1.f, "sRGB format"};
        default: return {0, 1.f, "SDR"};
    }
}

// A queue-idle fence does not cover WSI wait consumption. Keep retired waits until device teardown.


struct VulkanOverlay {
    VkDevice device{};
    VkExtent2D extent{};
    Encoding encoding{};
    std::vector<VkImage> images;   // the swap chain's
    // The menu is drawn by ImGui into an 8-bit image of its own (`menu_*`, `draw_pass`), then laid over the swap chain
    // image in its encoding (`compose_*`, menu_compose.frag): the same menu on SDR, sRGB-format and HDR swap chains.
    VkImage menu_image{};
    VkDeviceMemory menu_memory{};
    VkImageView menu_view{};
    VkFramebuffer menu_frame{};
    VkRenderPass draw_pass{}, compose_pass{};
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> frames;
    VkSampler sampler{};
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet set{};
    VkPipelineLayout layout{};
    VkPipeline compose{};
    ImGuiContext* context{};
    bool backend = false;
    Menu menu_ui;
    VkPhysicalDevice physical{};
    VkQueue queue{};
    VkCommandPool pool{};
    struct Submit { VkCommandBuffer cmd{}; VkFence fence{}; VkSemaphore done{}; };
    std::vector<Submit> submits;
    VkFence last{};
    VkBuffer readback{};
    VkDeviceMemory readmem{};
    void* mapped{};
    VkFormat format{};
    bool failed{};
    void init(VkInstance instance, VkPhysicalDevice phys, VkDevice dev, VkQueue q, uint32_t family,
              VkFormat fmt, VkColorSpaceKHR space, VkExtent2D ext, const std::vector<VkImage>& imgs) {
        device=dev; physical=phys; queue=q; format=fmt; extent=ext; images=imgs;
        encoding=encoding_of(format, space); context=menu_ui.context;
        ImGui::SetCurrentContext(context);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex=family;
        vk_check(vkCreateCommandPool(device,&pci,nullptr,&pool),"menu command pool");
        submits.resize(images.size());
        for(auto& f:submits) {
            VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
            vk_check(vkAllocateCommandBuffers(device,&ca,&f.cmd),"menu command buffer");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fi.flags=VK_FENCE_CREATE_SIGNALED_BIT;
            vk_check(vkCreateFence(device,&fi,nullptr,&f.fence),"menu fence");
            VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vk_check(vkCreateSemaphore(device,&si,nullptr,&f.done),"menu semaphore");
            nr::pe::receipts::watch(device, f.done);
        }
        // the menu's own image
        constexpr VkFormat kMenuFormat = VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = kMenuFormat; ii.extent = {extent.width, extent.height, 1};
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vk_check(vkCreateImage(device, &ii, nullptr, &menu_image), "menu image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, menu_image, &req);
        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        uint32_t type = mem.memoryTypeCount;
        for (uint32_t i = 0; i < mem.memoryTypeCount && type == mem.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
        for (uint32_t i = 0; i < mem.memoryTypeCount && type == mem.memoryTypeCount; ++i)
            if (req.memoryTypeBits & (1u << i)) type = i;
        if (type == mem.memoryTypeCount) throw std::runtime_error("no memory type for the menu image");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        vk_check(vkAllocateMemory(device, &ai, nullptr, &menu_memory), "menu memory");
        vk_check(vkBindImageMemory(device, menu_image, menu_memory, 0), "menu memory bind");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = menu_image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = kMenuFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(device, &vi, nullptr, &menu_view), "menu view");

        // ImGui draws into it from transparent black; the composite pass reads it
        VkAttachmentDescription a{};
        a.format = kMenuFormat; a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference colour{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &colour;
        VkSubpassDependency deps[2]{};
        // after the previous frame's composite has read it; before this frame's composite reads it
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; deps[0].srcAccessMask = 0;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &a; rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 2; rp.pDependencies = deps;
        vk_check(vkCreateRenderPass(device, &rp, nullptr, &draw_pass), "menu render pass");
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = draw_pass; fi.attachmentCount = 1; fi.pAttachments = &menu_view;
        fi.width = extent.width; fi.height = extent.height; fi.layers = 1;
        vk_check(vkCreateFramebuffer(device, &fi, nullptr, &menu_frame), "menu framebuffer");

        // the composite pass: over the swap chain image as the game left it
        a.format = format;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        rp.dependencyCount = 0; rp.pDependencies = nullptr;
        vk_check(vkCreateRenderPass(device, &rp, nullptr, &compose_pass), "composite render pass");
        for (VkImage image : images) {
            vi.image = image; vi.format = format;
            VkImageView view{};
            vk_check(vkCreateImageView(device, &vi, nullptr, &view), "image view");
            views.push_back(view);
            fi.renderPass = compose_pass; fi.pAttachments = &views.back();
            VkFramebuffer fb{};
            vk_check(vkCreateFramebuffer(device, &fi, nullptr, &fb), "framebuffer");
            frames.push_back(fb);
        }

        // the composite pipeline
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk_check(vkCreateSampler(device, &si, nullptr, &sampler), "sampler");
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1; li.pBindings = &binding;
        vk_check(vkCreateDescriptorSetLayout(device, &li, nullptr, &set_layout), "descriptor set layout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &size;
        vk_check(vkCreateDescriptorPool(device, &pi, nullptr, &descriptors), "descriptor pool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptors; da.descriptorSetCount = 1; da.pSetLayouts = &set_layout;
        vk_check(vkAllocateDescriptorSets(device, &da, &set), "descriptor set");
        VkDescriptorImageInfo image_info{sampler, menu_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &set_layout; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
        vk_check(vkCreatePipelineLayout(device, &pl, nullptr, &layout), "pipeline layout");
        VkShaderModule modules[2]{};
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof kMenuComposeVert; mi.pCode = kMenuComposeVert;
        vk_check(vkCreateShaderModule(device, &mi, nullptr, &modules[0]), "vertex shader");
        mi.codeSize = sizeof kMenuComposeFrag; mi.pCode = kMenuComposeFrag;
        const VkResult fr = vkCreateShaderModule(device, &mi, nullptr, &modules[1]);
        if (fr != VK_SUCCESS) { vkDestroyShaderModule(device, modules[0], nullptr); vk_check(fr, "fragment shader"); }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = modules[0]; stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = modules[1]; stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, 1};
        VkRect2D scissor{{0, 0}, extent};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.pViewports = &viewport; vp.scissorCount = 1; vp.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;   // premultiplied over the game's colour; the game's alpha kept
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &blend;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.layout = layout; gp.renderPass = compose_pass; gp.subpass = 0;
        const VkResult pr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &compose);
        vkDestroyShaderModule(device, modules[0], nullptr);
        vkDestroyShaderModule(device, modules[1], nullptr);
        vk_check(pr, "composite pipeline");

        // Resolve through the same PE Vulkan dispatch as the NR runtime.
        ImGui_ImplVulkan_InitInfo init{};
        init.ApiVersion = VK_API_VERSION_1_3;
        init.Instance = instance; init.PhysicalDevice = physical; init.Device = device;
        init.QueueFamily = family; init.Queue = queue; init.RenderPass = draw_pass;
        init.MinImageCount = 2; init.ImageCount = std::max(2u, uint32_t(images.size()));
        init.MSAASamples = VK_SAMPLE_COUNT_1_BIT; init.DescriptorPoolSize = 32;
        init.CheckVkResultFn = [](VkResult r) { vk_check(r, "ImGui Vulkan"); };
        if (!ImGui_ImplVulkan_Init(&init)) throw std::runtime_error("ImGui Vulkan initialization failed");
        backend = true;
    }
    ~VulkanOverlay() {
        // The caller proved command completion. WSI waits have a separate lifetime.
        for (auto& f : submits) {
            if (f.fence) vkDestroyFence(device, f.fence, nullptr);
            if (f.done) nr::pe::receipts::retire(device, f.done);
        }
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (readback) vkDestroyBuffer(device, readback, nullptr);
        if (readmem) { vkUnmapMemory(device, readmem); vkFreeMemory(device, readmem, nullptr); }
        {
            if (context) {
                ImGui::SetCurrentContext(context);
                if (backend) ImGui_ImplVulkan_Shutdown();
            }
        }
        if (compose) vkDestroyPipeline(device, compose, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (descriptors) vkDestroyDescriptorPool(device, descriptors, nullptr);
        if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        for (auto f : frames) vkDestroyFramebuffer(device, f, nullptr);
        for (auto v : views) vkDestroyImageView(device, v, nullptr);
        if (menu_frame) vkDestroyFramebuffer(device, menu_frame, nullptr);
        if (compose_pass) vkDestroyRenderPass(device, compose_pass, nullptr);
        if (draw_pass) vkDestroyRenderPass(device, draw_pass, nullptr);
        if (menu_view) vkDestroyImageView(device, menu_view, nullptr);
        if (menu_image) vkDestroyImage(device, menu_image, nullptr);
        if (menu_memory) vkFreeMemory(device, menu_memory, nullptr);
    }

    bool input() {
        ImGui::SetCurrentContext(context);
        return menu_ui.input(extent.width, extent.height);
    }
    void record(VkCommandBuffer cmd, uint32_t index) {
        menu_ui.fonts_for(extent.height);
        ImGui_ImplVulkan_NewFrame();
        menu_ui.build(extent.height);
        ImGui::SetCurrentContext(context);
        VkClearValue clear{};
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = draw_pass; rb.framebuffer = menu_frame; rb.renderArea = {{0, 0}, extent};
        rb.clearValueCount = 1; rb.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vkCmdEndRenderPass(cmd);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image = images[index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &b);
        rb.renderPass = compose_pass; rb.framebuffer = frames[index];
        rb.clearValueCount = 0; rb.pClearValues = nullptr;
        vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        const struct { uint32_t mode; float scale; } push{encoding.mode, encoding.scale};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &b);
    }
};


std::map<VkSwapchainKHR, std::unique_ptr<VulkanOverlay>> g_vulkan;

DXGI_FORMAT dump_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}
bool vk_dump_record(VulkanOverlay& o, VkCommandBuffer cmd, uint32_t index) {
    if (!dump_wanted() || dump_format(o.format) == DXGI_FORMAT_UNKNOWN) return false;
    const uint32_t bpp = o.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    if (!o.readback) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = VkDeviceSize(o.extent.width)*o.extent.height*bpp; bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vk_check(vkCreateBuffer(o.device,&bi,nullptr,&o.readback),"menu dump buffer");
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(o.device,o.readback,&req);
        VkPhysicalDeviceMemoryProperties mem{}; vkGetPhysicalDeviceMemoryProperties(o.physical,&mem);
        uint32_t type=mem.memoryTypeCount;
        const auto want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for(uint32_t i=0;i<mem.memoryTypeCount;++i)
            if((req.memoryTypeBits&(1u<<i))&&(mem.memoryTypes[i].propertyFlags&want)==want){type=i;break;}
        if(type==mem.memoryTypeCount)throw std::runtime_error("menu dump memory unavailable");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=type;
        vk_check(vkAllocateMemory(o.device,&ai,nullptr,&o.readmem),"menu dump memory");
        vk_check(vkBindBufferMemory(o.device,o.readback,o.readmem,0),"menu dump bind");
        vk_check(vkMapMemory(o.device,o.readmem,0,VK_WHOLE_SIZE,0,&o.mapped),"menu dump map");
    }
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image=o.images[index]; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={o.extent.width,o.extent.height,1};
    vkCmdCopyImageToBuffer(cmd,o.images[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,o.readback,1,&copy);
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=0;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&b);
    return true;
}
