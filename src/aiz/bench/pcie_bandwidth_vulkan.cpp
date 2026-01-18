#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_VULKAN

#include <aiz/dyn/vulkan.h>

#include <vulkan/vulkan.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace aiz {
namespace {

static const ::aiz::dyn::vulkan::Api* vkApi(std::string& err) {
  const auto* vk = ::aiz::dyn::vulkan::api(&err);
  return vk;
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

static bool findQueueFamily(VkPhysicalDevice phys, uint32_t& qfamOut) {
  std::string err;
  const auto* vk = vkApi(err);
  if (!vk) return false;

  uint32_t count = 0;
  vk->vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
  if (count == 0) return false;
  std::vector<VkQueueFamilyProperties> props(count);
  vk->vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, props.data());

  // Prefer a dedicated transfer queue; otherwise any queue.
  for (uint32_t i = 0; i < count; ++i) {
    if ((props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
        !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
        props[i].queueCount > 0) {
      qfamOut = i;
      return true;
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    if ((props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && props[i].queueCount > 0) {
      qfamOut = i;
      return true;
    }
  }
  return false;
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

struct BufferAlloc {
  VkBuffer buf{};
  VkDeviceMemory mem{};
};

static bool createBuffer(VkDevice dev,
                         VkPhysicalDevice phys,
                         VkDeviceSize size,
                         VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags memProps,
                         BufferAlloc& out,
                         std::string& err) {
  const auto* vk = vkApi(err);
  if (!vk) return false;

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkResult r = vk->vkCreateBuffer(dev, &bci, nullptr, &out.buf);
  if (r != VK_SUCCESS) {
    err = "vkCreateBuffer failed: " + vkErrToString(r);
    return false;
  }

  VkMemoryRequirements req{};
  vk->vkGetBufferMemoryRequirements(dev, out.buf, &req);

  auto mt = findMemType(phys, req.memoryTypeBits, memProps);
  if (!mt) {
    err = "No suitable Vulkan memory type.";
    vk->vkDestroyBuffer(dev, out.buf, nullptr);
    out.buf = VK_NULL_HANDLE;
    return false;
  }

  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = *mt;
  r = vk->vkAllocateMemory(dev, &mai, nullptr, &out.mem);
  if (r != VK_SUCCESS) {
    err = "vkAllocateMemory failed: " + vkErrToString(r);
    vk->vkDestroyBuffer(dev, out.buf, nullptr);
    out.buf = VK_NULL_HANDLE;
    return false;
  }

  r = vk->vkBindBufferMemory(dev, out.buf, out.mem, 0);
  if (r != VK_SUCCESS) {
    err = "vkBindBufferMemory failed: " + vkErrToString(r);
    vk->vkFreeMemory(dev, out.mem, nullptr);
    vk->vkDestroyBuffer(dev, out.buf, nullptr);
    out.mem = VK_NULL_HANDLE;
    out.buf = VK_NULL_HANDLE;
    return false;
  }

  return true;
}

class VulkanPcieBandwidth final : public IBenchmark {
public:
  explicit VulkanPcieBandwidth(unsigned int gpuIndex) : gpuIndex_(gpuIndex) {}

  std::string name() const override { return "Vulkan PCIe bandwidth"; }

  bool isAvailable() const override {
    const auto n = cachedVulkanPhysicalDeviceCount();
    return n && (*n > 0) && (gpuIndex_ < *n);
  }

  BenchResult run() override {
    std::string err;
    const auto* vk = vkApi(err);
    if (!vk) return BenchResult{false, err};

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

    uint32_t qfam = 0;
    if (!findQueueFamily(pd, qfam)) {
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "No Vulkan transfer-capable queue family found."};
    }

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

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = qfam;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};
    r = vk->vkCreateCommandPool(dev, &cpci, nullptr, &pool);
    if (r != VK_SUCCESS) {
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
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkAllocateCommandBuffers failed: " + vkErrToString(r)};
    }

    constexpr VkDeviceSize bytes = 256ull * 1024ull * 1024ull;
    BufferAlloc staging;
    if (!createBuffer(dev, pd, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, err)) {
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, err};
    }

    BufferAlloc deviceBuf;
    if (!createBuffer(dev, pd, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, deviceBuf, err)) {
      vk->vkFreeMemory(dev, staging.mem, nullptr);
      vk->vkDestroyBuffer(dev, staging.buf, nullptr);
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, err};
    }

    // Touch staging memory so it's backed.
    void* mapped = nullptr;
    r = vk->vkMapMemory(dev, staging.mem, 0, bytes, 0, &mapped);
    if (r != VK_SUCCESS || !mapped) {
      vk->vkFreeMemory(dev, deviceBuf.mem, nullptr);
      vk->vkDestroyBuffer(dev, deviceBuf.buf, nullptr);
      vk->vkFreeMemory(dev, staging.mem, nullptr);
      vk->vkDestroyBuffer(dev, staging.buf, nullptr);
      vk->vkDestroyCommandPool(dev, pool, nullptr);
      vk->vkDestroyDevice(dev, nullptr);
      vk->vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkMapMemory failed: " + vkErrToString(r)};
    }
    std::memset(mapped, 0xA5, static_cast<std::size_t>(bytes));
    vk->vkUnmapMemory(dev, staging.mem);

    auto measureCopy = [&](bool h2d, double& gbpsOut) -> bool {
      constexpr int warmup = 2;
      constexpr int iters = 10;

      auto doCopyOnce = [&]() -> std::optional<double> {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vk->vkResetCommandBuffer(cb, 0);
        if (vk->vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return std::nullopt;

        VkBufferCopy region{};
        region.size = bytes;

        if (h2d) {
          vk->vkCmdCopyBuffer(cb, staging.buf, deviceBuf.buf, 1, &region);
        } else {
          vk->vkCmdCopyBuffer(cb, deviceBuf.buf, staging.buf, 1, &region);
        }

        if (vk->vkEndCommandBuffer(cb) != VK_SUCCESS) return std::nullopt;

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;

        const auto t0 = std::chrono::steady_clock::now();
        VkResult sr = vk->vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
        if (sr != VK_SUCCESS) return std::nullopt;
        vk->vkQueueWaitIdle(q);
        const auto t1 = std::chrono::steady_clock::now();

        const double sec = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
        return sec;
      };

      for (int i = 0; i < warmup; ++i) {
        auto s = doCopyOnce();
        if (!s) {
          err = "Vulkan copy warmup failed.";
          return false;
        }
      }

      double total = 0.0;
      for (int i = 0; i < iters; ++i) {
        auto s = doCopyOnce();
        if (!s || *s <= 0.0) {
          err = "Vulkan copy timing failed.";
          return false;
        }
        total += *s;
      }
      const double avg = total / static_cast<double>(iters);
      gbpsOut = (static_cast<double>(bytes) / avg) / 1e9;
      return true;
    };

    double rx = 0.0;
    double tx = 0.0;
    const bool okRx = measureCopy(true, rx);
    const bool okTx = okRx ? measureCopy(false, tx) : false;

  vk->vkFreeMemory(dev, deviceBuf.mem, nullptr);
  vk->vkDestroyBuffer(dev, deviceBuf.buf, nullptr);
  vk->vkFreeMemory(dev, staging.mem, nullptr);
  vk->vkDestroyBuffer(dev, staging.buf, nullptr);

  vk->vkDestroyCommandPool(dev, pool, nullptr);
  vk->vkDestroyDevice(dev, nullptr);
  vk->vkDestroyInstance(inst, nullptr);

    if (!okRx || !okTx) return BenchResult{false, err.empty() ? std::string("copy failed") : err};

    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << "RX: " << rx << " GB/s, TX: " << tx << " GB/s";
    return BenchResult{true, out.str()};
  }

private:
  unsigned int gpuIndex_ = 0;
};

}  // namespace

std::unique_ptr<IBenchmark> makeGpuPcieBandwidthBenchmarkVulkanBackend(unsigned int gpuIndex) {
  return std::make_unique<VulkanPcieBandwidth>(gpuIndex);
}

}  // namespace aiz

#endif  // AI_Z_ENABLE_VULKAN
