#pragma once

#include <string>

#ifdef AI_Z_ENABLE_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace aiz::dyn::vulkan {

#ifdef AI_Z_ENABLE_VULKAN

struct Api {
  void* handle = nullptr;

  PFN_vkCreateInstance vkCreateInstance = nullptr;
  PFN_vkDestroyInstance vkDestroyInstance = nullptr;
  PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;

  PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;

  PFN_vkCreateDevice vkCreateDevice = nullptr;
  PFN_vkDestroyDevice vkDestroyDevice = nullptr;
  PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;

  PFN_vkCreateBuffer vkCreateBuffer = nullptr;
  PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkFreeMemory vkFreeMemory = nullptr;
  PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
  PFN_vkMapMemory vkMapMemory = nullptr;
  PFN_vkUnmapMemory vkUnmapMemory = nullptr;

  PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
  PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
  PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;

  PFN_vkCmdCopyBuffer vkCmdCopyBuffer = nullptr;
  PFN_vkQueueSubmit vkQueueSubmit = nullptr;
  PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;

  PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
  PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
  PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
  PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
  PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
  PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
  PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
  PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;

  PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
  PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
  PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
  PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;

  PFN_vkCreateQueryPool vkCreateQueryPool = nullptr;
  PFN_vkDestroyQueryPool vkDestroyQueryPool = nullptr;
  PFN_vkCmdWriteTimestamp vkCmdWriteTimestamp = nullptr;
  PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
  PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
  PFN_vkCmdDispatch vkCmdDispatch = nullptr;

  PFN_vkCreateFence vkCreateFence = nullptr;
  PFN_vkDestroyFence vkDestroyFence = nullptr;
  PFN_vkWaitForFences vkWaitForFences = nullptr;

  PFN_vkGetQueryPoolResults vkGetQueryPoolResults = nullptr;
};

const Api* api(std::string* errOut = nullptr);

#endif  // AI_Z_ENABLE_VULKAN

}  // namespace aiz::dyn::vulkan
