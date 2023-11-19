#include <stdexcept>

#define DECLARE(func) PFN_vk##func func = (PFN_vk##func)GetInstanceProcAddr(*pInstance, "vk" #func);
//#define FORWARD(func) dispatchTable.func = (PFN_vk##func)gpa(*pInstance, "vk" #func);

struct PvkInstanceDispatchTable {
  VkInstance *pInstance;
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
  PvkInstanceDispatchTable(VkInstance *pInstance, PFN_vkGetInstanceProcAddr gpa):
    pInstance(pInstance), GetInstanceProcAddr(gpa) {
  }
  PvkInstanceDispatchTable() {
    throw std::logic_error("Accessing un-fetched instance dispatch table");
  }
  DECLARE(EnumeratePhysicalDevices);
  DECLARE(DestroyInstance);
  DECLARE(EnumerateDeviceExtensionProperties);
  DECLARE(GetPhysicalDeviceProperties);
  DECLARE(GetPhysicalDeviceMemoryProperties);
  DECLARE(GetPhysicalDeviceQueueFamilyProperties);
#ifdef VK_USE_PLATFORM_XCB_KHR
  DECLARE(GetPhysicalDeviceXcbPresentationSupportKHR);
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
  DECLARE(GetPhysicalDeviceXlibPresentationSupportKHR);
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
  DECLARE(GetPhysicalDeviceWaylandPresentationSupportKHR);
#endif
  DECLARE(GetPhysicalDeviceSurfaceSupportKHR);
#define FORWARD(func) DECLARE(func)
#include "primus_vk_forwarding.h"
#undef FORWARD

};
#undef DECLARE

#define DECLARE(func) PFN_vk##func func = (PFN_vk##func)GetDeviceProcAddr(*pDevice, "vk" #func);
struct PvkDispatchTable {
  VkDevice *pDevice;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr;

  PvkDispatchTable(VkDevice *pDevice, PFN_vkGetDeviceProcAddr gdpa):
    pDevice(pDevice), GetDeviceProcAddr(gdpa){
  }
  PvkDispatchTable() {
    throw std::logic_error("Accessing un-fetched dispatch table");
  }
  
  DECLARE(DestroyDevice);

  DECLARE(CreateSwapchainKHR);
  DECLARE(DestroySwapchainKHR);
  DECLARE(GetSwapchainImagesKHR);
  DECLARE(AcquireNextImageKHR);
  DECLARE(GetSwapchainStatusKHR);
  DECLARE(QueuePresentKHR);

  DECLARE(CreateImage);
  DECLARE(GetImageMemoryRequirements);
  DECLARE(AllocateMemory);
  DECLARE(BindImageMemory);
  DECLARE(GetImageSubresourceLayout);
  DECLARE(FreeMemory);
  DECLARE(DestroyImage);
  DECLARE(MapMemory);
  DECLARE(UnmapMemory);


  DECLARE(AllocateCommandBuffers);
  DECLARE(BeginCommandBuffer);
  DECLARE(CmdDraw);
  DECLARE(CmdDrawIndexed);
  DECLARE(CmdCopyImage);
  DECLARE(CmdPipelineBarrier);
  DECLARE(CreateCommandPool);
  //DECLARE(CreateDevice);
  DECLARE(EndCommandBuffer);
  //DECLARE(EnumeratePhysicalDevices);
  DECLARE(FreeCommandBuffers);
  DECLARE(DestroyCommandPool);
  //DECLARE(GetPhysicalDeviceMemoryProperties);
  DECLARE(QueueSubmit);
  DECLARE(DeviceWaitIdle);
  DECLARE(QueueWaitIdle);

  DECLARE(GetDeviceQueue);

  DECLARE(CreateFence);
  DECLARE(WaitForFences);
  DECLARE(ResetFences);
  DECLARE(DestroyFence);

  DECLARE(CreateSemaphore);
  DECLARE(DestroySemaphore);

  DECLARE(InvalidateMappedMemoryRanges);

};

#undef DECLARE
