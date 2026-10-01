#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <iterator>
struct Ctx{VkInstance inst;VkPhysicalDevice gpu;VkDevice dev;VkQueue q;uint32_t qf;VkPhysicalDeviceMemoryProperties mp;};
static Ctx mk(){
 Ctx c{}; VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion=VK_API_VERSION_1_3;
 VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo=&ai; vkCreateInstance(&ic,0,&c.inst);
 uint32_t n=0; vkEnumeratePhysicalDevices(c.inst,&n,0); std::vector<VkPhysicalDevice> pd(n); vkEnumeratePhysicalDevices(c.inst,&n,pd.data());
 for(auto d:pd){VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d,&p); if(p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)c.gpu=d;}
 float q=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount=1;qi.pQueuePriorities=&q;
 uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(c.gpu,&qn,0); std::vector<VkQueueFamilyProperties> qp(qn); vkGetPhysicalDeviceQueueFamilyProperties(c.gpu,&qn,qp.data());
 for(uint32_t i=0;i<qn;i++) if(qp[i].queueFlags&VK_QUEUE_COMPUTE_BIT){qi.queueFamilyIndex=i;break;} c.qf=qi.queueFamilyIndex;
 VkPhysicalDeviceCooperativeMatrixFeaturesKHR cf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR}; cf.cooperativeMatrix=1;
 VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES}; f12.shaderFloat16=1; f12.vulkanMemoryModel=1; f12.storageBuffer8BitAccess=1; f12.pNext=&cf;
 VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES}; f11.storageBuffer16BitAccess=1; f11.pNext=&f12;
 VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.subgroupSizeControl=1; f13.pNext=&f11;
 const char* ext[]={"VK_KHR_cooperative_matrix"};
 VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.pNext=&f13; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qi; dc.enabledExtensionCount=1; dc.ppEnabledExtensionNames=ext;
 if(vkCreateDevice(c.gpu,&dc,0,&c.dev)){puts("devfail");exit(1);} vkGetDeviceQueue(c.dev,c.qf,0,&c.q); vkGetPhysicalDeviceMemoryProperties(c.gpu,&c.mp); return c;}
struct Buf{VkBuffer b;VkDeviceMemory m;void* p;size_t n;};
static Buf mkbuf(Ctx&c,size_t n){Buf r{};r.n=n; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=n; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; vkCreateBuffer(c.dev,&bi,0,&r.b);
 VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c.dev,r.b,&mr); uint32_t mt=0; auto want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
 for(uint32_t i=0;i<c.mp.memoryTypeCount;i++) if((mr.memoryTypeBits&(1<<i))&&(c.mp.memoryTypes[i].propertyFlags&want)==want){mt=i;break;}
 VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize=mr.size; ma.memoryTypeIndex=mt; vkAllocateMemory(c.dev,&ma,0,&r.m); vkBindBufferMemory(c.dev,r.b,r.m,0); vkMapMemory(c.dev,r.m,0,n,0,&r.p); return r;}
static void run(Ctx&c,const char* spv,std::vector<Buf>&bufs,uint32_t gx){
 std::ifstream f(spv,std::ios::binary); std::vector<char> code((std::istreambuf_iterator<char>(f)),{});
 VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=code.size(); sm.pCode=(uint32_t*)code.data(); VkShaderModule mod; vkCreateShaderModule(c.dev,&sm,0,&mod);
 std::vector<VkDescriptorSetLayoutBinding> bs; for(uint32_t i=0;i<bufs.size();i++) bs.push_back({i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,0});
 VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=bs.size(); dl.pBindings=bs.data(); VkDescriptorSetLayout dsl; vkCreateDescriptorSetLayout(c.dev,&dl,0,&dsl);
 VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&dsl; VkPipelineLayout lay; vkCreatePipelineLayout(c.dev,&pl,0,&lay);
 VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO}; rs.requiredSubgroupSize=32;
 VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pc.layout=lay; pc.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,&rs,0,VK_SHADER_STAGE_COMPUTE_BIT,mod,"main",0};
 VkPipeline pipe; if(vkCreateComputePipelines(c.dev,0,1,&pc,0,&pipe)){puts("pipefail");exit(1);}
 VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,(uint32_t)bufs.size()}; VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&ps; VkDescriptorPool pool; vkCreateDescriptorPool(c.dev,&dp,0,&pool);
 VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=pool;da.descriptorSetCount=1;da.pSetLayouts=&dsl; VkDescriptorSet ds; vkAllocateDescriptorSets(c.dev,&da,&ds);
 std::vector<VkDescriptorBufferInfo> bi(bufs.size()); std::vector<VkWriteDescriptorSet> ws; for(uint32_t i=0;i<bufs.size();i++){bi[i]={bufs[i].b,0,VK_WHOLE_SIZE}; VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=ds;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&bi[i]; ws.push_back(w);} vkUpdateDescriptorSets(c.dev,ws.size(),ws.data(),0,0);
 VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex=c.qf; VkCommandPool cpool; vkCreateCommandPool(c.dev,&cpi,0,&cpool);
 VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cba.commandPool=cpool;cba.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cba.commandBufferCount=1; VkCommandBuffer cb; vkAllocateCommandBuffers(c.dev,&cba,&cb);
 VkCommandBufferBeginInfo bg{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(cb,&bg);
 vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe); vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,lay,0,1,&ds,0,0); vkCmdDispatch(cb,gx,1,1); vkEndCommandBuffer(cb);
 VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb; vkQueueSubmit(c.q,1,&si,0); vkQueueWaitIdle(c.q);}
