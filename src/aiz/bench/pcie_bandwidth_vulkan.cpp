#include <aiz/bench/bench.h>

#ifdef AI_Z_ENABLE_VULKAN

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
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
  if (count == 0) return false;
  std::vector<VkQueueFamilyProperties> props(count);
  vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, props.data());

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
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mp);
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
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ai-z";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "ai-z";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;

    VkInstance inst{};
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
      cached = std::nullopt;
      return;
    }

    uint32_t n = 0;
    const VkResult r = vkEnumeratePhysicalDevices(inst, &n, nullptr);
    vkDestroyInstance(inst, nullptr);
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
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkResult r = vkCreateBuffer(dev, &bci, nullptr, &out.buf);
  if (r != VK_SUCCESS) {
    err = "vkCreateBuffer failed: " + vkErrToString(r);
    return false;
  }

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(dev, out.buf, &req);

  auto mt = findMemType(phys, req.memoryTypeBits, memProps);
  if (!mt) {
    err = "No suitable Vulkan memory type.";
    vkDestroyBuffer(dev, out.buf, nullptr);
    out.buf = VK_NULL_HANDLE;
    return false;
  }

  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = *mt;
  r = vkAllocateMemory(dev, &mai, nullptr, &out.mem);
  if (r != VK_SUCCESS) {
    err = "vkAllocateMemory failed: " + vkErrToString(r);
    vkDestroyBuffer(dev, out.buf, nullptr);
    out.buf = VK_NULL_HANDLE;
    return false;
  }

  r = vkBindBufferMemory(dev, out.buf, out.mem, 0);
  if (r != VK_SUCCESS) {
    err = "vkBindBufferMemory failed: " + vkErrToString(r);
    vkFreeMemory(dev, out.mem, nullptr);
    vkDestroyBuffer(dev, out.buf, nullptr);
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
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ai-z";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "ai-z";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst{};
    VkResult r = vkCreateInstance(&ici, nullptr, &inst);
    if (r != VK_SUCCESS) return BenchResult{false, "vkCreateInstance failed: " + vkErrToString(r)};

    uint32_t physCount = 0;
    r = vkEnumeratePhysicalDevices(inst, &physCount, nullptr);
    if (r != VK_SUCCESS || physCount == 0) {
      vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "No Vulkan physical devices found."};
    }
    if (gpuIndex_ >= physCount) {
      vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "Invalid Vulkan GPU index."};
    }

    std::vector<VkPhysicalDevice> phys(physCount);
    vkEnumeratePhysicalDevices(inst, &physCount, phys.data());
    VkPhysicalDevice pd = phys[gpuIndex_];

    uint32_t qfam = 0;
    if (!findQueueFamily(pd, qfam)) {
      vkDestroyInstance(inst, nullptr);
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
    r = vkCreateDevice(pd, &dci, nullptr, &dev);
    if (r != VK_SUCCESS) {
      vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateDevice failed: " + vkErrToString(r)};
    }

    VkQueue q{};
    vkGetDeviceQueue(dev, qfam, 0, &q);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = qfam;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};
    r = vkCreateCommandPool(dev, &cpci, nullptr, &pool);
    if (r != VK_SUCCESS) {
      vkDestroyDevice(dev, nullptr);
      vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkCreateCommandPool failed: " + vkErrToString(r)};
    }

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb{};
    r = vkAllocateCommandBuffers(dev, &cbai, &cb);
    if (r != VK_SUCCESS) {
      vkDestroyCommandPool(dev, pool, nullptr);
      vkDestroyDevice(dev, nullptr);
      vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkAllocateCommandBuffers failed: " + vkErrToString(r)};
    }

    constexpr VkDeviceSize bytes = 256ull * 1024ull * 1024ull;
    std::string err;

    BufferAlloc staging;
    if (!createBuffer(dev, pd, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, err)) {
      vkDestroyCommandPool(dev, pool, nullptr);
      vkDestroyDevice(dev, nullptr);
      vkDestroyInstance(inst, nullptr);
      return BenchResult{false, err};
    }

    BufferAlloc deviceBuf;
    if (!createBuffer(dev, pd, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, deviceBuf, err)) {
      vkFreeMemory(dev, staging.mem, nullptr);
      vkDestroyBuffer(dev, staging.buf, nullptr);
      vkDestroyCommandPool(dev, pool, nullptr);
      vkDestroyDevice(dev, nullptr);
      vkDestroyInstance(inst, nullptr);
      return BenchResult{false, err};
    }

    // Touch staging memory so it's backed.
    void* mapped = nullptr;
    r = vkMapMemory(dev, staging.mem, 0, bytes, 0, &mapped);
    if (r != VK_SUCCESS || !mapped) {
      vkFreeMemory(dev, deviceBuf.mem, nullptr);
      vkDestroyBuffer(dev, deviceBuf.buf, nullptr);
      vkFreeMemory(dev, staging.mem, nullptr);
      vkDestroyBuffer(dev, staging.buf, nullptr);
      vkDestroyCommandPool(dev, pool, nullptr);
      vkDestroyDevice(dev, nullptr);
      vkDestroyInstance(inst, nullptr);
      return BenchResult{false, "vkMapMemory failed: " + vkErrToString(r)};
    }
    std::memset(mapped, 0xA5, static_cast<std::size_t>(bytes));
    vkUnmapMemory(dev, staging.mem);

    auto measureCopy = [&](bool h2d, double& gbpsOut) -> bool {
      constexpr int warmup = 2;
      constexpr int iters = 10;

      auto doCopyOnce = [&]() -> std::optional<double> {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkResetCommandBuffer(cb, 0);
        if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return std::nullopt;

        VkBufferCopy region{};
        region.size = bytes;

        if (h2d) {
          vkCmdCopyBuffer(cb, staging.buf, deviceBuf.buf, 1, &region);
        } else {
          vkCmdCopyBuffer(cb, deviceBuf.buf, staging.buf, 1, &region);
        }

        if (vkEndCommandBuffer(cb) != VK_SUCCESS) return std::nullopt;

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;

        const auto t0 = std::chrono::steady_clock::now();
        VkResult sr = vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
        if (sr != VK_SUCCESS) return std::nullopt;
        vkQueueWaitIdle(q);
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

    vkFreeMemory(dev, deviceBuf.mem, nullptr);
    vkDestroyBuffer(dev, deviceBuf.buf, nullptr);
    vkFreeMemory(dev, staging.mem, nullptr);
    vkDestroyBuffer(dev, staging.buf, nullptr);

    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);

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
