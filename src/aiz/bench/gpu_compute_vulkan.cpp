#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_VULKAN

#include "vulkan_fp32_bench_spv.h"

#include <aiz/dyn/vulkan.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <mutex>
#include <string>
#include <vector>

namespace aiz {
namespace {

static const ::aiz::dyn::vulkan::Api* vkApi(std::string& err) {
  return ::aiz::dyn::vulkan::api(&err);
}

static std::string vkErrToString(VkResult r) {
  switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_EVENT_SET: return "VK_EVENT_SET";
    case VK_EVENT_RESET: return "VK_EVENT_RESET";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: break;
  }
  return "VK_ERROR(" + std::to_string(static_cast<int>(r)) + ")";
}

static std::optional<uint32_t> findQueueFamilyCompute(VkPhysicalDevice phys) {
  std::string err;
  const auto* vk = vkApi(err);
  if (!vk) return std::nullopt;

  uint32_t count = 0;
  vk->vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
  if (count == 0) return std::nullopt;
  std::vector<VkQueueFamilyProperties> props(count);
  vk->vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, props.data());

  // Prefer compute-only if available.
  for (uint32_t i = 0; i < count; ++i) {
    if ((props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && props[i].queueCount > 0) {
      if (props[i].timestampValidBits == 0) continue;
      return i;
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    if ((props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && props[i].queueCount > 0) {
      if (props[i].timestampValidBits == 0) continue;
      return i;
    }
  }
  return std::nullopt;
}

static std::optional<uint32_t> findMemType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags want) {
  std::string err;
  const auto* vk = vkApi(err);
  if (!vk) return std::nullopt;

  VkPhysicalDeviceMemoryProperties mp{};
  vk->vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) == 0) continue;
    if ((mp.memoryTypes[i].propertyFlags & want) == want) return i;
  }
  return std::nullopt;
}

static std::optional<uint32_t> cachedVulkanPhysicalDeviceCount() {
  static std::once_flag once;
  static std::optional<uint32_t> cached;

  std::call_once(once, []() {
    std::string err;
    const auto* vk = vkApi(err);
    if (!vk) {
      cached = std::nullopt;
      return;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ai-z";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "ai-z";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;

    VkInstance inst{};
    if (vk->vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
      cached = std::nullopt;
      return;
    }

    uint32_t n = 0;
    const VkResult r = vk->vkEnumeratePhysicalDevices(inst, &n, nullptr);
    vk->vkDestroyInstance(inst, nullptr);
    if (r != VK_SUCCESS) {
      cached = std::nullopt;
      return;
    }
    cached = n;
  });

  return cached;
}

class VulkanFp32Compute final : public IBenchmark {
public:
  explicit VulkanFp32Compute(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}

  std::string name() const override { return "Vulkan FLOPS FP32"; }

  bool isAvailable() const override {
    const auto n = cachedVulkanPhysicalDeviceCount();
    return n && (*n > 0) && (gpuIndex_ < *n);
  }

  BenchResult run() override {
    std::string err;
    const auto* vk = vkApi(err);
    if (!vk) return BenchResult{false, err};

    if (bench::kVulkanFp32BenchSpvWordCount == 0) {
      return BenchResult{false, "Embedded SPIR-V is empty."};
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ai-z";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "ai-z";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst{};
    VkResult r = vk->vkCreateInstance(&ici, nullptr, &inst);
    if (r != VK_SUCCESS) return BenchResult{false, "vkCreateInstance failed: " + vkErrToString(r)};

    uint32_t physCount = 0;
    r = vk->vkEnumeratePhysicalDevices(inst, &physCount, nullptr);
    if (r != VK_SUCCESS || physCount == 0) {
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "No Vulkan physical devices found."};
    }
    if (gpuIndex_ >= physCount) {
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "Invalid Vulkan GPU index."};
    }
    std::vector<VkPhysicalDevice> phys(physCount);
    vk->vkEnumeratePhysicalDevices(inst, &physCount, phys.data());
    VkPhysicalDevice pd = phys[gpuIndex_];

    VkPhysicalDeviceProperties props{};
    vk->vkGetPhysicalDeviceProperties(pd, &props);
    if (props.limits.timestampPeriod <= 0.0f) {
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "Vulkan device does not report a valid timestampPeriod."};
    }

    auto qfamOpt = findQueueFamilyCompute(pd);
    if (!qfamOpt) {
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "No Vulkan compute queue family with timestamp support found."};
    }
    const uint32_t qfam = *qfamOpt;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice dev{};
    r = vk->vkCreateDevice(pd, &dci, nullptr, &dev);
    if (r != VK_SUCCESS) {
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateDevice failed: " + vkErrToString(r)};
    }

    VkQueue q{};
    vk->vkGetDeviceQueue(dev, qfam, 0, &q);

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = bench::kVulkanFp32BenchSpvWordCount * sizeof(std::uint32_t);
    smci.pCode = bench::kVulkanFp32BenchSpv;
    VkShaderModule shader{};
    r = vk->vkCreateShaderModule(dev, &smci, nullptr, &shader);
    if (r != VK_SUCCESS) {
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateShaderModule failed: " + vkErrToString(r)};
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsli.bindingCount = 1;
    dsli.pBindings = &binding;
    VkDescriptorSetLayout dsl{};
    r = vk->vkCreateDescriptorSetLayout(dev, &dsli, nullptr, &dsl);
    if (r != VK_SUCCESS) {
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateDescriptorSetLayout failed: " + vkErrToString(r)};
    }

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    VkPipelineLayout pl{};
    r = vk->vkCreatePipelineLayout(dev, &plci, nullptr, &pl);
    if (r != VK_SUCCESS) {
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreatePipelineLayout failed: " + vkErrToString(r)};
    }

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = stage;
    cpci.layout = pl;
    VkPipeline pipe{};
    r = vk->vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe);
    if (r != VK_SUCCESS) {
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateComputePipelines failed: " + vkErrToString(r)};
    }

    // Output buffer.
    constexpr uint32_t kLocalSize = 256;
    constexpr uint32_t kN = 1u << 20;
    constexpr uint32_t kIters = 4096;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(kN) * sizeof(float);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf{};
    r = vk->vkCreateBuffer(dev, &bci, nullptr, &buf);
    if (r != VK_SUCCESS) {
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateBuffer failed: " + vkErrToString(r)};
    }

    VkMemoryRequirements req{};
    vk->vkGetBufferMemoryRequirements(dev, buf, &req);
    auto mt = findMemType(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!mt) {
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "No suitable Vulkan device-local memory type for output buffer."};
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = *mt;
    VkDeviceMemory mem{};
    r = vk->vkAllocateMemory(dev, &mai, nullptr, &mem);
    if (r != VK_SUCCESS) {
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkAllocateMemory failed: " + vkErrToString(r)};
    }
    r = vk->vkBindBufferMemory(dev, buf, mem, 0);
    if (r != VK_SUCCESS) {
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkBindBufferMemory failed: " + vkErrToString(r)};
    }

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VkDescriptorPool dp{};
    r = vk->vkCreateDescriptorPool(dev, &dpci, nullptr, &dp);
    if (r != VK_SUCCESS) {
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateDescriptorPool failed: " + vkErrToString(r)};
    }

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dp;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet ds{};
    r = vk->vkAllocateDescriptorSets(dev, &dsai, &ds);
    if (r != VK_SUCCESS) {
      vk->vkDestroyDescriptorPool(dev, dp, nullptr);
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkAllocateDescriptorSets failed: " + vkErrToString(r)};
    }

    VkDescriptorBufferInfo dbi{};
    dbi.buffer = buf;
    dbi.offset = 0;
    dbi.range = bytes;
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet = ds;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds.pBufferInfo = &dbi;
    vk->vkUpdateDescriptorSets(dev, 1, &wds, 0, nullptr);

    VkCommandPoolCreateInfo cpci2{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci2.queueFamilyIndex = qfam;
    cpci2.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};
    r = vk->vkCreateCommandPool(dev, &cpci2, nullptr, &pool);
    if (r != VK_SUCCESS) {
      vk->vkDestroyDescriptorPool(dev, dp, nullptr);
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateCommandPool failed: " + vkErrToString(r)};
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb{};
    r = vk->vkAllocateCommandBuffers(dev, &cbai, &cb);
    if (r != VK_SUCCESS) {
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDescriptorPool(dev, dp, nullptr);
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkAllocateCommandBuffers failed: " + vkErrToString(r)};
    }

    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;
    VkQueryPool qpool{};
    r = vk->vkCreateQueryPool(dev, &qpci, nullptr, &qpool);
    if (r != VK_SUCCESS) {
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDescriptorPool(dev, dp, nullptr);
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateQueryPool failed: " + vkErrToString(r)};
    }

    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vk->vkBeginCommandBuffer(cb, &cbi);
    if (r != VK_SUCCESS) {
      vk->vkDestroyQueryPool(dev, qpool, nullptr);
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDescriptorPool(dev, dp, nullptr);
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkBeginCommandBuffer failed: " + vkErrToString(r)};
    }

    vk->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 0);
    vk->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vk->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
    const uint32_t groups = (kN + kLocalSize - 1) / kLocalSize;
    vk->vkCmdDispatch(cb, groups, 1, 1);
    vk->vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, qpool, 1);

    r = vk->vkEndCommandBuffer(cb);
    if (r != VK_SUCCESS) {
      vk->vkDestroyQueryPool(dev, qpool, nullptr);
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDescriptorPool(dev, dp, nullptr);
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkEndCommandBuffer failed: " + vkErrToString(r)};
    }

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{};
    r = vk->vkCreateFence(dev, &fci, nullptr, &fence);
    if (r != VK_SUCCESS) {
      vk->vkDestroyQueryPool(dev, qpool, nullptr);
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDescriptorPool(dev, dp, nullptr);
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateFence failed: " + vkErrToString(r)};
    }

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    r = vk->vkQueueSubmit(q, 1, &si, fence);
    if (r != VK_SUCCESS) {
      vk->vkDestroyFence(dev, fence, nullptr);
      vk->vkDestroyQueryPool(dev, qpool, nullptr);
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDescriptorPool(dev, dp, nullptr);
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkQueueSubmit failed: " + vkErrToString(r)};
    }

    r = vk->vkWaitForFences(dev, 1, &fence, VK_TRUE, 60ull * 1000ull * 1000ull * 1000ull);
    if (r != VK_SUCCESS) {
      vk->vkDestroyFence(dev, fence, nullptr);
      vk->vkDestroyQueryPool(dev, qpool, nullptr);
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDescriptorPool(dev, dp, nullptr);
      vk->vkFreeMemory(dev, mem, nullptr);
      vk->vkDestroyBuffer(dev, buf, nullptr);
      vk->vkDestroyPipeline(dev, pipe, nullptr);
      vk->vkDestroyPipelineLayout(dev, pl, nullptr);
      vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
      vk->vkDestroyShaderModule(dev, shader, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkWaitForFences failed/timeout: " + vkErrToString(r)};
    }

    uint64_t ts[2] = {0, 0};
    r = vk->vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    vk->vkDestroyFence(dev, fence, nullptr);
    vk->vkDestroyQueryPool(dev, qpool, nullptr);

    // Cleanup.
    vk->vkDestroyCommandPool(dev, pool, nullptr);
    vk->vkDestroyDescriptorPool(dev, dp, nullptr);
    vk->vkFreeMemory(dev, mem, nullptr);
    vk->vkDestroyBuffer(dev, buf, nullptr);
    vk->vkDestroyPipeline(dev, pipe, nullptr);
    vk->vkDestroyPipelineLayout(dev, pl, nullptr);
    vk->vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    vk->vkDestroyShaderModule(dev, shader, nullptr);
    vk->vkDestroyDevice(dev, nullptr);
    vk->vkDestroyInstance(inst, nullptr);

    if (r != VK_SUCCESS) return BenchResult{false, "vkGetQueryPoolResults failed: " + vkErrToString(r)};
    if (ts[1] <= ts[0]) return BenchResult{false, "Invalid Vulkan timestamp delta."};

    const double ns = static_cast<double>(ts[1] - ts[0]) * static_cast<double>(props.limits.timestampPeriod);
    const double sec = ns * 1e-9;
    if (sec <= 0.0) return BenchResult{false, "Timing failed."};

    const double flops = static_cast<double>(kN) * static_cast<double>(kIters) * 2.0;
    const double gflops = (flops / sec) / 1e9;

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(2);
    oss << gflops << " GFLOPS";
    return BenchResult{true, oss.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

}  // namespace

std::unique_ptr<IBenchmark> makeGpuFp32BenchmarkVulkanBackend(unsigned int gpuIndex) {
  return std::make_unique<VulkanFp32Compute>(gpuIndex);
}

}  // namespace aiz

#endif  // AI_Z_ENABLE_VULKAN
