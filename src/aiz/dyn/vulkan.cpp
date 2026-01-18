#include <aiz/dyn/vulkan.h>

#ifdef AI_Z_ENABLE_VULKAN

#include <dlfcn.h>

#include <mutex>
#include <string>

namespace aiz::dyn::vulkan {
namespace {

template <typename T>
static bool loadRequired(void* handle, const char* name, T& fn, std::string& err) {
  dlerror();
  void* sym = dlsym(handle, name);
  const char* e = dlerror();
  if (e != nullptr || sym == nullptr) {
    err = std::string("Missing Vulkan symbol '") + name + "': " + (e ? e : "(null)");
    return false;
  }
  fn = reinterpret_cast<T>(sym);
  return true;
}

static const char* kCandidates[] = {
    "libvulkan.so.1",
    "libvulkan.so",
};

static std::once_flag g_once;
static Api g_api;
static std::string g_err;
static bool g_ok = false;

static void initOnce() {
  void* handle = nullptr;
  for (const char* cand : kCandidates) {
    handle = dlopen(cand, RTLD_LAZY | RTLD_LOCAL);
    if (handle) break;
  }

  if (!handle) {
    const char* e = dlerror();
    g_err = std::string("Vulkan runtime not found (dlopen libvulkan.so failed): ") + (e ? e : "(null)");
    g_ok = false;
    return;
  }

  Api api;
  api.handle = handle;

  if (!loadRequired(handle, "vkCreateInstance", api.vkCreateInstance, g_err) ||
      !loadRequired(handle, "vkDestroyInstance", api.vkDestroyInstance, g_err) ||
      !loadRequired(handle, "vkEnumeratePhysicalDevices", api.vkEnumeratePhysicalDevices, g_err) ||
      !loadRequired(handle, "vkGetPhysicalDeviceQueueFamilyProperties", api.vkGetPhysicalDeviceQueueFamilyProperties, g_err) ||
      !loadRequired(handle, "vkGetPhysicalDeviceMemoryProperties", api.vkGetPhysicalDeviceMemoryProperties, g_err) ||
      !loadRequired(handle, "vkGetPhysicalDeviceProperties", api.vkGetPhysicalDeviceProperties, g_err) ||
      !loadRequired(handle, "vkCreateDevice", api.vkCreateDevice, g_err) ||
      !loadRequired(handle, "vkDestroyDevice", api.vkDestroyDevice, g_err) ||
      !loadRequired(handle, "vkGetDeviceQueue", api.vkGetDeviceQueue, g_err) ||
      !loadRequired(handle, "vkCreateBuffer", api.vkCreateBuffer, g_err) ||
      !loadRequired(handle, "vkDestroyBuffer", api.vkDestroyBuffer, g_err) ||
      !loadRequired(handle, "vkGetBufferMemoryRequirements", api.vkGetBufferMemoryRequirements, g_err) ||
      !loadRequired(handle, "vkAllocateMemory", api.vkAllocateMemory, g_err) ||
      !loadRequired(handle, "vkFreeMemory", api.vkFreeMemory, g_err) ||
      !loadRequired(handle, "vkBindBufferMemory", api.vkBindBufferMemory, g_err) ||
      !loadRequired(handle, "vkMapMemory", api.vkMapMemory, g_err) ||
      !loadRequired(handle, "vkUnmapMemory", api.vkUnmapMemory, g_err) ||
      !loadRequired(handle, "vkCreateCommandPool", api.vkCreateCommandPool, g_err) ||
      !loadRequired(handle, "vkDestroyCommandPool", api.vkDestroyCommandPool, g_err) ||
      !loadRequired(handle, "vkAllocateCommandBuffers", api.vkAllocateCommandBuffers, g_err) ||
      !loadRequired(handle, "vkResetCommandBuffer", api.vkResetCommandBuffer, g_err) ||
      !loadRequired(handle, "vkBeginCommandBuffer", api.vkBeginCommandBuffer, g_err) ||
      !loadRequired(handle, "vkEndCommandBuffer", api.vkEndCommandBuffer, g_err) ||
      !loadRequired(handle, "vkCmdCopyBuffer", api.vkCmdCopyBuffer, g_err) ||
      !loadRequired(handle, "vkQueueSubmit", api.vkQueueSubmit, g_err) ||
      !loadRequired(handle, "vkQueueWaitIdle", api.vkQueueWaitIdle, g_err) ||
      !loadRequired(handle, "vkCreateShaderModule", api.vkCreateShaderModule, g_err) ||
      !loadRequired(handle, "vkDestroyShaderModule", api.vkDestroyShaderModule, g_err) ||
      !loadRequired(handle, "vkCreateDescriptorSetLayout", api.vkCreateDescriptorSetLayout, g_err) ||
      !loadRequired(handle, "vkDestroyDescriptorSetLayout", api.vkDestroyDescriptorSetLayout, g_err) ||
      !loadRequired(handle, "vkCreatePipelineLayout", api.vkCreatePipelineLayout, g_err) ||
      !loadRequired(handle, "vkDestroyPipelineLayout", api.vkDestroyPipelineLayout, g_err) ||
      !loadRequired(handle, "vkCreateComputePipelines", api.vkCreateComputePipelines, g_err) ||
      !loadRequired(handle, "vkDestroyPipeline", api.vkDestroyPipeline, g_err) ||
      !loadRequired(handle, "vkCreateDescriptorPool", api.vkCreateDescriptorPool, g_err) ||
      !loadRequired(handle, "vkDestroyDescriptorPool", api.vkDestroyDescriptorPool, g_err) ||
      !loadRequired(handle, "vkAllocateDescriptorSets", api.vkAllocateDescriptorSets, g_err) ||
      !loadRequired(handle, "vkUpdateDescriptorSets", api.vkUpdateDescriptorSets, g_err) ||
      !loadRequired(handle, "vkCreateQueryPool", api.vkCreateQueryPool, g_err) ||
      !loadRequired(handle, "vkDestroyQueryPool", api.vkDestroyQueryPool, g_err) ||
      !loadRequired(handle, "vkCmdWriteTimestamp", api.vkCmdWriteTimestamp, g_err) ||
      !loadRequired(handle, "vkCmdBindPipeline", api.vkCmdBindPipeline, g_err) ||
      !loadRequired(handle, "vkCmdBindDescriptorSets", api.vkCmdBindDescriptorSets, g_err) ||
      !loadRequired(handle, "vkCmdDispatch", api.vkCmdDispatch, g_err) ||
      !loadRequired(handle, "vkCreateFence", api.vkCreateFence, g_err) ||
      !loadRequired(handle, "vkDestroyFence", api.vkDestroyFence, g_err) ||
      !loadRequired(handle, "vkWaitForFences", api.vkWaitForFences, g_err) ||
      !loadRequired(handle, "vkGetQueryPoolResults", api.vkGetQueryPoolResults, g_err)) {
    dlclose(handle);
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
