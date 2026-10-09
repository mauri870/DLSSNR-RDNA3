// Matrix throughput of the int8 16x16x16 cooperative-matrix multiply-add as the driver compiles it (v_wmma_i32_16x16x16_iu8)
// and with that opcode rewritten to the iu4 form, in the pipeline binary, as the int4 mixed network does (nrvk.hpp
// rewrite_iu4). run.sh builds the shaders this loads (i1, i2: int8 with 5000 and 20000 loop iterations).
// The rewritten kernel computes garbage (an iu4 WMMA reads half of the operand registers), which a rate does not care about.
// On an RX 7900 XTX iu8 runs at 127 TOPS, as f16 does; iu4 at about twice that: 16x16x16 of 4-bit in the time of 8-bit.
// gfx12 has a 16x16x32 iu4 form of the same opcode instead (0xcc4a); only the gfx11 form is measured here.
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_) { std::printf("%s failed: %d\n", #x, r_); std::exit(1); } } while (0)

struct Device { VkInstance instance; VkPhysicalDevice gpu; VkDevice device; VkQueue queue; uint32_t family; VkPhysicalDeviceMemoryProperties memory; };

static Device make_device() {
    Device d{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ic, nullptr, &d.instance));
    uint32_t n = 0; vkEnumeratePhysicalDevices(d.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> all(n); vkEnumeratePhysicalDevices(d.instance, &n, all.data());
    for (auto g : all) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(g, &p); if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) d.gpu = g; }
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(d.gpu, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn); vkGetPhysicalDeviceQueueFamilyProperties(d.gpu, &qn, qp.data());
    for (uint32_t i = 0; i < qn; ++i) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { d.family = i; break; }
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex = d.family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR}; m5.maintenance5 = 1;
    VkPhysicalDevicePipelineBinaryFeaturesKHR pb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR}; pb.pipelineBinaries = 1; pb.pNext = &m5;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR}; cm.cooperativeMatrix = 1; cm.pNext = &pb;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.shaderFloat16 = 1; f12.vulkanMemoryModel = 1; f12.storageBuffer8BitAccess = 1; f12.shaderInt8 = 1; f12.pNext = &cm;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES}; f11.storageBuffer16BitAccess = 1; f11.pNext = &f12;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.subgroupSizeControl = 1; f13.pNext = &f11;
    const char* extensions[] = {"VK_KHR_cooperative_matrix", "VK_KHR_pipeline_binary", "VK_KHR_maintenance5"};
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.pNext = &f13; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qi;
    dc.enabledExtensionCount = 3; dc.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateDevice(d.gpu, &dc, nullptr, &d.device));
    vkGetDeviceQueue(d.device, d.family, 0, &d.queue);
    vkGetPhysicalDeviceMemoryProperties(d.gpu, &d.memory);
    return d;
}

struct Buffer { VkBuffer buffer; VkDeviceMemory memory; void* mapped; size_t bytes; };

static Buffer make_buffer(const Device& d, size_t bytes) {
    Buffer b{}; b.bytes = bytes;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = bytes; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    CHECK(vkCreateBuffer(d.device, &bi, nullptr, &b.buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(d.device, b.buffer, &req);
    const auto want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = 0;
    for (uint32_t i = 0; i < d.memory.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize = req.size; ma.memoryTypeIndex = type;
    CHECK(vkAllocateMemory(d.device, &ma, nullptr, &b.memory));
    CHECK(vkBindBufferMemory(d.device, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(d.device, b.memory, 0, bytes, 0, &b.mapped));
    return b;
}

// The best of a few timed submissions of `workgroups` workgroups of the kernel in `spv_path`; with `iu4` its int8 WMMAs
// are rewritten to the iu4 form (the same encoding test as nrvk.hpp rewrite_iu4) through a pipeline binary.
static double time_ms(const Device& d, const char* spv_path, bool iu4, uint32_t workgroups, const Buffer& out, const Buffer& in) {
    std::ifstream file(spv_path, std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
    if (code.empty()) { std::printf("cannot read %s\n", spv_path); std::exit(1); }
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize = code.size(); sm.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module; CHECK(vkCreateShaderModule(d.device, &sm, nullptr, &module));
    VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 2; dl.pBindings = bindings;
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(d.device, &dl, nullptr, &set_layout));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &set_layout;
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(d.device, &pl, nullptr, &layout));
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO}; subgroup.requiredSubgroupSize = 32;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.layout = layout;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &subgroup, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    VkPipeline pipeline{};
    VkPipelineBinaryInfoKHR binary_info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR};
    std::vector<VkPipelineBinaryKHR> binaries;
    if (!iu4) {
        CHECK(vkCreateComputePipelines(d.device, nullptr, 1, &ci, nullptr, &pipeline));
    } else {
#define LOAD(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(d.device, #name))
        LOAD(vkCreatePipelineBinariesKHR); LOAD(vkGetPipelineBinaryDataKHR); LOAD(vkDestroyPipelineBinaryKHR); LOAD(vkReleaseCapturedPipelineDataKHR);
#undef LOAD
        VkPipelineCreateFlags2CreateInfoKHR flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR}; flags.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
        VkComputePipelineCreateInfo capture_ci = ci; capture_ci.pNext = &flags;
        VkPipeline captured; CHECK(vkCreateComputePipelines(d.device, nullptr, 1, &capture_ci, nullptr, &captured));
        VkPipelineBinaryCreateInfoKHR bci{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR}; bci.pipeline = captured;
        VkPipelineBinaryHandlesInfoKHR handles{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
        CHECK(vkCreatePipelineBinariesKHR(d.device, &bci, nullptr, &handles));
        std::vector<VkPipelineBinaryKHR> captured_binaries(handles.pipelineBinaryCount); handles.pPipelineBinaries = captured_binaries.data();
        CHECK(vkCreatePipelineBinariesKHR(d.device, &bci, nullptr, &handles));
        VkReleaseCapturedPipelineDataInfoKHR release{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR}; release.pipeline = captured;
        vkReleaseCapturedPipelineDataKHR(d.device, &release, nullptr);
        vkDestroyPipeline(d.device, captured, nullptr);
        std::vector<std::vector<uint8_t>> data(captured_binaries.size());
        std::vector<VkPipelineBinaryKeyKHR> keys(captured_binaries.size());
        std::vector<VkPipelineBinaryDataKHR> blobs(captured_binaries.size());
        int rewritten = 0;
        for (size_t i = 0; i < captured_binaries.size(); ++i) {
            VkPipelineBinaryDataInfoKHR gi{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR}; gi.pipelineBinary = captured_binaries[i];
            keys[i] = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
            size_t size = 0;
            CHECK(vkGetPipelineBinaryDataKHR(d.device, &gi, &keys[i], &size, nullptr));
            data[i].resize(size);
            CHECK(vkGetPipelineBinaryDataKHR(d.device, &gi, &keys[i], &size, data[i].data()));
            for (size_t o = 0; o + 8 <= size; o += 4) {
                uint32_t d1, d2; std::memcpy(&d1, &data[i][o], 4); std::memcpy(&d2, &data[i][o + 4], 4);
                if ((d1 >> 16) != 0xcc44u || (d1 & 0x7f00u) != 0x4000u || (d2 >> 29) != 3u) continue;
                d1 = (d1 & 0xffffu) | (0xcc45u << 16);   // v_wmma_i32_16x16x16_iu8 -> _iu4
                std::memcpy(&data[i][o], &d1, 4); ++rewritten; o += 4;
            }
            for (uint32_t k = 0; k < keys[i].keySize; ++k) keys[i].key[k] ^= 0x5a;   // never aliases the original
            blobs[i] = {size, data[i].data()};
            vkDestroyPipelineBinaryKHR(d.device, captured_binaries[i], nullptr);
        }
        if (!rewritten) { std::printf("no WMMA to rewrite in %s\n", spv_path); std::exit(1); }
        VkPipelineBinaryKeysAndDataKHR kd{uint32_t(blobs.size()), keys.data(), blobs.data()};
        VkPipelineBinaryCreateInfoKHR bci2{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR}; bci2.pKeysAndDataInfo = &kd;
        VkPipelineBinaryHandlesInfoKHR handles2{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
        binaries.resize(blobs.size()); handles2.pipelineBinaryCount = uint32_t(binaries.size()); handles2.pPipelineBinaries = binaries.data();
        CHECK(vkCreatePipelineBinariesKHR(d.device, &bci2, nullptr, &handles2));
        binary_info.binaryCount = uint32_t(binaries.size()); binary_info.pPipelineBinaries = binaries.data();
        ci.pNext = &binary_info;
        CHECK(vkCreateComputePipelines(d.device, nullptr, 1, &ci, nullptr, &pipeline));
    }
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = 1; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(d.device, &dp, nullptr, &pool));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool = pool; da.descriptorSetCount = 1; da.pSetLayouts = &set_layout;
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(d.device, &da, &set));
    VkDescriptorBufferInfo infos[2] = {{out.buffer, 0, VK_WHOLE_SIZE}, {in.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(d.device, 2, writes, 0, nullptr);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex = d.family;
    VkCommandPool command_pool; CHECK(vkCreateCommandPool(d.device, &cp, nullptr, &command_pool));
    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cba.commandPool = command_pool; cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cba.commandBufferCount = 1;
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(d.device, &cba, &cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd, &begin));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, workgroups, 1, 1);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    double best = 1e30;
    for (int repeat = 0; repeat < 5; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        CHECK(vkQueueSubmit(d.queue, 1, &submit, nullptr)); CHECK(vkQueueWaitIdle(d.queue));
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    return best;
}

int main() {
    const Device d = make_device();
    const uint32_t workgroups = 96 * 8;   // 96 compute units, eight workgroups each
    const Buffer out = make_buffer(d, size_t(workgroups) * 512 * 4 + 4096), in = make_buffer(d, 1 << 16);
    std::memset(in.mapped, 1, in.bytes); std::memset(out.mapped, 0, out.bytes);
    // 128 invocations a workgroup = 4 waves; 8 independent accumulators a wave; 2*16*16*16 operations a multiply-add.
    const double operations = double(workgroups) * 4 * 8 * 2 * 16 * 16 * 16 * (20000 - 5000);
    for (int repeat = 0; repeat < 3; ++repeat) {
        const double a1 = time_ms(d, "i1.spv", false, workgroups, out, in), a2 = time_ms(d, "i2.spv", false, workgroups, out, in);
        const double b1 = time_ms(d, "i1.spv", true, workgroups, out, in), b2 = time_ms(d, "i2.spv", true, workgroups, out, in);
        std::printf("iu8: %.1f ms -> %.1f ms  %.1f TOPS | iu4 (16x16x16): %.1f ms -> %.1f ms  %.1f TOPS\n",
                    a1, a2, operations / ((a2 - a1) * 1e-3) / 1e12, b1, b2, operations / ((b2 - b1) * 1e-3) / 1e12);
    }
}
