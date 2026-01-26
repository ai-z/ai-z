#include <aiz/dyn/vulkan.h>

#ifdef AI_Z_ENABLE_VULKAN

#include <aiz/platform/dynlib.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aiz::dyn::vulkan {
namespace {

template <typename T>
static bool loadRequired(platform::DynamicLibrary& lib, const char* name, T& fn, std::string& err) {
  if (!lib.loadSymbol(name, fn)) {
    err = std::string("Missing Vulkan symbol '") + name + "'";
    return false;
  }
  return true;
}

static std::once_flag g_once;
static Api g_api;
static std::string g_err;
static bool g_ok = false;
static std::unique_ptr<platform::DynamicLibrary> g_lib;

static void initOnce() {
  std::vector<const char*> candidates;
  candidates.push_back(platform::vulkanLibraryName());
#if defined(AI_Z_PLATFORM_LINUX)
  candidates.push_back("libvulkan.so");
#endif

  g_lib = platform::loadLibrary(candidates, &g_err);
  if (!g_lib || !g_lib->isValid()) {
    if (g_err.empty()) g_err = "Vulkan runtime not found";
    g_ok = false;
    return;
  }

  Api api;
  api.handle = g_lib.get();

  if (!loadRequired(*g_lib, "vkCreateInstance", api.vkCreateInstance, g_err) ||
      !loadRequired(*g_lib, "vkDestroyInstance", api.vkDestroyInstance, g_err) ||
      !loadRequired(*g_lib, "vkEnumeratePhysicalDevices", api.vkEnumeratePhysicalDevices, g_err) ||
      !loadRequired(*g_lib, "vkGetPhysicalDeviceQueueFamilyProperties", api.vkGetPhysicalDeviceQueueFamilyProperties, g_err) ||
      !loadRequired(*g_lib, "vkGetPhysicalDeviceMemoryProperties", api.vkGetPhysicalDeviceMemoryProperties, g_err) ||
      !loadRequired(*g_lib, "vkGetPhysicalDeviceProperties", api.vkGetPhysicalDeviceProperties, g_err) ||
      !loadRequired(*g_lib, "vkCreateDevice", api.vkCreateDevice, g_err) ||
      !loadRequired(*g_lib, "vkDestroyDevice", api.vkDestroyDevice, g_err) ||
      !loadRequired(*g_lib, "vkGetDeviceQueue", api.vkGetDeviceQueue, g_err) ||
      !loadRequired(*g_lib, "vkCreateBuffer", api.vkCreateBuffer, g_err) ||
      !loadRequired(*g_lib, "vkDestroyBuffer", api.vkDestroyBuffer, g_err) ||
      !loadRequired(*g_lib, "vkGetBufferMemoryRequirements", api.vkGetBufferMemoryRequirements, g_err) ||
      !loadRequired(*g_lib, "vkAllocateMemory", api.vkAllocateMemory, g_err) ||
      !loadRequired(*g_lib, "vkFreeMemory", api.vkFreeMemory, g_err) ||
      !loadRequired(*g_lib, "vkBindBufferMemory", api.vkBindBufferMemory, g_err) ||
      !loadRequired(*g_lib, "vkMapMemory", api.vkMapMemory, g_err) ||
      !loadRequired(*g_lib, "vkUnmapMemory", api.vkUnmapMemory, g_err) ||
      !loadRequired(*g_lib, "vkCreateCommandPool", api.vkCreateCommandPool, g_err) ||
      !loadRequired(*g_lib, "vkDestroyCommandPool", api.vkDestroyCommandPool, g_err) ||
      !loadRequired(*g_lib, "vkAllocateCommandBuffers", api.vkAllocateCommandBuffers, g_err) ||
      !loadRequired(*g_lib, "vkResetCommandBuffer", api.vkResetCommandBuffer, g_err) ||
      !loadRequired(*g_lib, "vkBeginCommandBuffer", api.vkBeginCommandBuffer, g_err) ||
      !loadRequired(*g_lib, "vkEndCommandBuffer", api.vkEndCommandBuffer, g_err) ||
      !loadRequired(*g_lib, "vkCmdCopyBuffer", api.vkCmdCopyBuffer, g_err) ||
      !loadRequired(*g_lib, "vkQueueSubmit", api.vkQueueSubmit, g_err) ||
      !loadRequired(*g_lib, "vkQueueWaitIdle", api.vkQueueWaitIdle, g_err) ||
      !loadRequired(*g_lib, "vkCreateShaderModule", api.vkCreateShaderModule, g_err) ||
      !loadRequired(*g_lib, "vkDestroyShaderModule", api.vkDestroyShaderModule, g_err) ||
      !loadRequired(*g_lib, "vkCreateDescriptorSetLayout", api.vkCreateDescriptorSetLayout, g_err) ||
      !loadRequired(*g_lib, "vkDestroyDescriptorSetLayout", api.vkDestroyDescriptorSetLayout, g_err) ||
      !loadRequired(*g_lib, "vkCreatePipelineLayout", api.vkCreatePipelineLayout, g_err) ||
      !loadRequired(*g_lib, "vkDestroyPipelineLayout", api.vkDestroyPipelineLayout, g_err) ||
      !loadRequired(*g_lib, "vkCreateComputePipelines", api.vkCreateComputePipelines, g_err) ||
      !loadRequired(*g_lib, "vkDestroyPipeline", api.vkDestroyPipeline, g_err) ||
      !loadRequired(*g_lib, "vkCreateDescriptorPool", api.vkCreateDescriptorPool, g_err) ||
      !loadRequired(*g_lib, "vkDestroyDescriptorPool", api.vkDestroyDescriptorPool, g_err) ||
      !loadRequired(*g_lib, "vkAllocateDescriptorSets", api.vkAllocateDescriptorSets, g_err) ||
      !loadRequired(*g_lib, "vkUpdateDescriptorSets", api.vkUpdateDescriptorSets, g_err) ||
      !loadRequired(*g_lib, "vkCreateQueryPool", api.vkCreateQueryPool, g_err) ||
      !loadRequired(*g_lib, "vkDestroyQueryPool", api.vkDestroyQueryPool, g_err) ||
      !loadRequired(*g_lib, "vkCmdWriteTimestamp", api.vkCmdWriteTimestamp, g_err) ||
      !loadRequired(*g_lib, "vkCmdBindPipeline", api.vkCmdBindPipeline, g_err) ||
      !loadRequired(*g_lib, "vkCmdBindDescriptorSets", api.vkCmdBindDescriptorSets, g_err) ||
      !loadRequired(*g_lib, "vkCmdDispatch", api.vkCmdDispatch, g_err) ||
      !loadRequired(*g_lib, "vkCreateFence", api.vkCreateFence, g_err) ||
      !loadRequired(*g_lib, "vkDestroyFence", api.vkDestroyFence, g_err) ||
      !loadRequired(*g_lib, "vkWaitForFences", api.vkWaitForFences, g_err) ||
      !loadRequired(*g_lib, "vkGetQueryPoolResults", api.vkGetQueryPoolResults, g_err)) {
    g_ok = false;
    return;
  }

  g_api = api;
  g_ok = true;
}

}  // namespace

const Api* api(std::string* errOut) {
  std::call_once(g_once, initOnce);
  if (!g_ok) {
    if (errOut) *errOut = g_err;
    return nullptr;
  }
  return &g_api;
}

}  // namespace aiz::dyn::vulkan

#endif  // AI_Z_ENABLE_VULKAN
