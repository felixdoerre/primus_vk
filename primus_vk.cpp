#include "vulkan.h"
#include "vk_layer.h"

#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>

#include <assert.h>
#include <string.h>

#include <mutex>
#include <condition_variable>
#include <map>
#include <vector>
#include <list>
#include <iostream>

#include <pthread.h>

#include <stdexcept>

#include <dlfcn.h>

#include <vector>
#include <memory>
#include <thread>

#undef VK_LAYER_EXPORT
#if defined(WIN32)
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define VK_LAYER_EXPORT extern "C"
#endif

// single global lock, for simplicity
std::mutex global_lock;
typedef std::lock_guard<std::mutex> scoped_lock;

// use the loader's dispatch table pointer as a key for dispatch map lookups
template<typename DispatchableType>
void *&GetKey(DispatchableType inst)
{
  return *(void **)inst;
}

struct InstanceInfo {
  VkInstance instance;
  VkPhysicalDevice render;
  VkPhysicalDevice display;
};

std::map<void *, VkLayerInstanceDispatchTable> instance_dispatch;
VkLayerInstanceDispatchTable loader_dispatch;
// VkInstance->disp is beeing malloc'ed for every new instance
// so we can assume it to be a good key.
std::map<void *, InstanceInfo> instance_info;
std::map<void *, VkLayerDispatchTable> device_dispatch;

std::shared_ptr<void> libvulkan(dlopen("libvulkan.so.1", RTLD_NOW), dlclose);


// #define TRACE(x)
#define TRACE(x) std::cout << "PrimusVK: " << x << "\n";
#define TRACE_PROFILING(x)
// #define TRACE_PROFILING(x) std::cout << "PrimusVK: " << x << "\n";
#define TRACE_FRAME(x)
// #define TRACE_FRAME(x) std::cout << "PrimusVK: " << x << "\n";
#define VK_CHECK_RESULT(x) do{ const VkResult r = x; if(r != VK_SUCCESS){printf("Error %d in %d\n", r, __LINE__);}}while(0);
// #define VK_CHECK_RESULT(x) if(x != VK_SUCCESS){printf("Error %d, in %d\n", x, __LINE__);}


///////////////////////////////////////////////////////////////////////////////////////////
// Layer init and shutdown
VkLayerDispatchTable fetchDispatchTable(PFN_vkGetDeviceProcAddr gdpa, VkDevice *pDevice);
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
  TRACE("CreateInstance");
  VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance)gpa(VK_NULL_HANDLE, "vkCreateInstance");

  VK_CHECK_RESULT( createFunc(pCreateInfo, pAllocator, pInstance) );

  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerInstanceDispatchTable dispatchTable;
#define FORWARD(func) dispatchTable.func = (PFN_vk##func)gpa(*pInstance, "vk" #func);
  FORWARD(GetInstanceProcAddr);
  FORWARD(EnumeratePhysicalDevices);
  FORWARD(DestroyInstance);
  FORWARD(EnumerateDeviceExtensionProperties);
  FORWARD(GetPhysicalDeviceProperties);
#undef FORWARD

  std::vector<VkPhysicalDevice> physicalDevices;
  {
    auto enumerateDevices = dispatchTable.EnumeratePhysicalDevices;
    TRACE("Getting devices");
    uint32_t gpuCount = 0;
    enumerateDevices(*pInstance, &gpuCount, nullptr);
    physicalDevices.resize(gpuCount);
    enumerateDevices(*pInstance, &gpuCount, physicalDevices.data());
  }
  VkPhysicalDevice display = VK_NULL_HANDLE;
  VkPhysicalDevice render = VK_NULL_HANDLE;
  for(auto &dev: physicalDevices){
    VkPhysicalDeviceProperties props;
    dispatchTable.GetPhysicalDeviceProperties(dev, &props);
    TRACE(GetKey(dev) << ": ");
    if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU){
      display = dev;
      TRACE("got display!");
    }
    if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU){
      TRACE("got render!");
      render = dev;
    }
    TRACE("Device: " << props.deviceName);
    TRACE("  Type: " << props.deviceType);
  }
  if(display == VK_NULL_HANDLE) {
    TRACE("No device for the display GPU found. Are the intel-mesa drivers installed?");
  }
  if(render == VK_NULL_HANDLE) {
    TRACE("No device for the rendering GPU found. Is the correct driver installed?");
  }
  if(display == VK_NULL_HANDLE || render == VK_NULL_HANDLE){
    return VK_ERROR_INITIALIZATION_FAILED;
  }
#define FORWARD(func) dispatchTable.func = (PFN_vk##func)gpa(*pInstance, "vk" #func);
  FORWARD(GetPhysicalDeviceSurfaceFormatsKHR);
  FORWARD(GetPhysicalDeviceQueueFamilyProperties);
  FORWARD(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  FORWARD(GetPhysicalDeviceSurfaceSupportKHR);
  FORWARD(GetPhysicalDeviceSurfacePresentModesKHR);
#undef FORWARD

#define _FORWARD(x) loader_dispatch.x =(PFN_vk##x) dlsym(libvulkan.get(), "vk" #x);
  _FORWARD(CreateDevice);
  _FORWARD(EnumeratePhysicalDevices);
  _FORWARD(GetPhysicalDeviceMemoryProperties);
  _FORWARD(GetPhysicalDeviceQueueFamilyProperties);
#undef _FORWARD

  // store the table by key
  {
    scoped_lock l(global_lock);

    instance_dispatch[GetKey(*pInstance)] = dispatchTable;
    instance_info[GetKey(*pInstance)] = InstanceInfo{.instance = *pInstance, .render = render, .display=display};
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  instance_dispatch.erase(GetKey(instance));
  instance_info.erase(GetKey(instance));
  // TODO call DestroyInstance down the chain?
}

struct FramebufferImage;
struct MappedMemory{
  VkDevice device;
  VkDeviceMemory mem;
  char* data;
  MappedMemory(VkDevice device, FramebufferImage &img);
  ~MappedMemory();
};
struct FramebufferImage {
  VkImage img;
  VkDeviceMemory mem;

  VkDevice device;

  std::shared_ptr<MappedMemory> mapped;
  FramebufferImage(VkDevice device, VkExtent2D size, VkImageTiling tiling, VkImageUsageFlags usage, int memoryTypeIndex): device(device){
    TRACE("Creating image: " << size.width << "x" << size.height);
    VkImageCreateInfo imageCreateCI {};
    imageCreateCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
    imageCreateCI.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageCreateCI.extent.width = size.width;
    imageCreateCI.extent.height = size.height;
    imageCreateCI.extent.depth = 1;
    imageCreateCI.arrayLayers = 1;
    imageCreateCI.mipLevels = 1;
    imageCreateCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateCI.tiling = tiling;
    imageCreateCI.usage = usage;
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].CreateImage(device, &imageCreateCI, nullptr, &img));

    VkMemoryRequirements memRequirements {};
    VkMemoryAllocateInfo memAllocInfo {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    device_dispatch[GetKey(device)].GetImageMemoryRequirements(device, img, &memRequirements);
    memAllocInfo.allocationSize = memRequirements.size;
    memAllocInfo.memoryTypeIndex = memoryTypeIndex;
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].AllocateMemory(device, &memAllocInfo, nullptr, &mem));
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].BindImageMemory(device, img, mem, 0));
  }
  std::shared_ptr<MappedMemory> getMapped(){
    if(!mapped){
      throw std::runtime_error("not mapped");
    }
    return mapped;
  }
  void map(){
    mapped = std::make_shared<MappedMemory>(device, *this);
  }
  VkSubresourceLayout getLayout(){
    VkImageSubresource subResource { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout subResourceLayout;
    device_dispatch[GetKey(device)].GetImageSubresourceLayout(device, img, &subResource, &subResourceLayout);
    return subResourceLayout;
  }
  ~FramebufferImage(){
    device_dispatch[GetKey(device)].FreeMemory(device, mem, nullptr);
    device_dispatch[GetKey(device)].DestroyImage(device, img, nullptr);
  }
};
MappedMemory::MappedMemory(VkDevice device, FramebufferImage &img): device(device), mem(img.mem){
  device_dispatch[GetKey(device)].MapMemory(device, img.mem, 0, VK_WHOLE_SIZE, 0, (void**)&data);
}
MappedMemory::~MappedMemory(){
  device_dispatch[GetKey(device)].UnmapMemory(device, mem);
}
class CommandBuffer;
class Fence{
  VkDevice device;
public:
  VkFence fence;
  Fence(VkDevice dev): device(dev){
    // Create fence to ensure that the command buffer has finished executing
    VkFenceCreateInfo fenceInfo = {.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags=0};
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].CreateFence(device, &fenceInfo, nullptr, &fence));
  }
  void await(){
    const auto start = std::chrono::steady_clock::now();
    // Wait for the fence to signal that command buffer has finished executing
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].WaitForFences(device, 1, &fence, VK_TRUE, 10000000000L));
    TRACE_PROFILING("Time for fence: " << std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count() << " seconds");
  }
  ~Fence(){
    device_dispatch[GetKey(device)].DestroyFence(device, fence, nullptr);
  }
};
class Semaphore{
  VkDevice device;
public:
  VkSemaphore sem;
  Semaphore(VkDevice dev): device(dev){
    VkSemaphoreCreateInfo semInfo = {.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .flags=0};
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].CreateSemaphore(device, &semInfo, nullptr, &sem));
  }
  ~Semaphore(){
    device_dispatch[GetKey(device)].DestroySemaphore(device, sem, nullptr);
  }
};
struct MySwapchain{
  std::chrono::steady_clock::time_point lastPresent = std::chrono::steady_clock::now();
  VkDevice device;
  VkQueue render_queue;
  VkDevice display_device;
  VkQueue display_queue;
  VkSwapchainKHR backend;
  std::vector<std::shared_ptr<FramebufferImage>> render_images;
  std::vector<std::shared_ptr<FramebufferImage>> render_copy_images;
  std::vector<std::shared_ptr<FramebufferImage>> display_src_images;
  std::vector<VkImage> display_images;
  VkExtent2D imgSize;

  std::vector<std::shared_ptr<CommandBuffer>> display_commands;

  std::unique_ptr<std::thread> myThread;
  MySwapchain(){
    TRACE("Creating a Swapchain thread.")
    myThread = std::unique_ptr<std::thread>(new std::thread([this](){this->run();}));
    pthread_setname_np(myThread->native_handle(), "swapchain-thread");
  }


  std::unique_ptr<Semaphore> sem;
  void copyImageData(uint32_t idx, std::vector<VkSemaphore> sems);
  void storeImage(uint32_t index, VkDevice device, VkExtent2D imgSize, VkQueue queue, VkFormat colorFormat, std::vector<VkSemaphore> wait_on, Fence &notify);

  void queue(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);

  std::mutex queueMutex;
  std::condition_variable has_work;
  bool active = true;
  struct QueueItem {
    VkQueue queue;
    VkPresentInfoKHR pPresentInfo;
    uint32_t imgIndex;
    std::unique_ptr<Fence> fence;
  };
  std::list<QueueItem> work;
  void present(const QueueItem &workItem);
  void run();
  void stop();
};

bool list_all_gpus = false;
void* threadmain(void *d);
class CreateOtherDevice {
public:
  VkPhysicalDevice display_dev;
  VkPhysicalDevice render_dev;
  VkPhysicalDeviceMemoryProperties display_mem;
  VkPhysicalDeviceMemoryProperties render_mem;
  VkDevice render_gpu;
  VkDevice display_gpu;

  std::unique_ptr<std::thread> myThread;
  CreateOtherDevice(VkPhysicalDevice display_dev, VkPhysicalDevice render_dev, VkDevice render_gpu):
    display_dev(display_dev), render_dev(render_dev), render_gpu(render_gpu){
  }
  void run(){
    auto &minstance_info = instance_info[GetKey(render_dev)];
    
    VkDevice pDeviceLogic;
    TRACE("Thread running");
    TRACE("getting rendering suff: " << GetKey(display_dev));
    uint32_t gpuCount;
    list_all_gpus = true;
    loader_dispatch.EnumeratePhysicalDevices(minstance_info.instance, &gpuCount, nullptr);
    TRACE("Gpus: " << gpuCount);
    std::vector<VkPhysicalDevice> physicalDevices(gpuCount);
    loader_dispatch.EnumeratePhysicalDevices(minstance_info.instance, &gpuCount, physicalDevices.data());
    list_all_gpus = false;

    display_dev = physicalDevices[1];
    TRACE("phys[1]: " << display_dev);

    loader_dispatch.GetPhysicalDeviceMemoryProperties(display_dev, &display_mem);
    loader_dispatch.GetPhysicalDeviceMemoryProperties(physicalDevices[0], &render_mem);

    std::vector<VkQueueFamilyProperties> queueFamilyProperties;
    if(true){
      PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueues = loader_dispatch.GetPhysicalDeviceQueueFamilyProperties;
      uint32_t queueFamilyCount;
      getQueues(display_dev, &queueFamilyCount, nullptr);
      assert(queueFamilyCount > 0);
      queueFamilyProperties.resize(queueFamilyCount);
      getQueues(display_dev, &queueFamilyCount, queueFamilyProperties.data());
      TRACE("render queues: " << queueFamilyCount);
      for(auto &props : queueFamilyProperties){
	TRACE(" flags: " << queueFamilyProperties[0].queueFlags);
      }
    }
    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = 0;
    queueInfo.queueCount = 1;
    const float defaultQueuePriority(0.0f);
    queueInfo.pQueuePriorities = &defaultQueuePriority;

    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = 1;
    const char *swap[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    createInfo.ppEnabledExtensionNames = swap;
    VkResult ret;

    TRACE("Creating Graphics: " << ret << ", ");
    ret = loader_dispatch.CreateDevice(display_dev, &createInfo, nullptr, &pDeviceLogic);
    TRACE("Create Graphics FINISHED!: " << ret);
    TRACE("Display: " << GetKey(pDeviceLogic));
    TRACE("storing as reference to: " << GetKey(render_gpu));
    display_gpu = pDeviceLogic;

  }

  void start(){
    myThread = std::unique_ptr<std::thread>(new std::thread([this](){this->run();}));
  }
  void join(){
    if(myThread == nullptr) { TRACE( "Refusing second join" ); return; }
    myThread->join();
    myThread.reset();
  }
};
void* threadmain(void *d){
  auto *p = reinterpret_cast<CreateOtherDevice*>(d);
  p->run();
  return nullptr;
}



class CommandBuffer {
  VkCommandPool commandPool;
  VkDevice device;
public:
  VkCommandBuffer cmd;
  CommandBuffer(VkDevice device) : device(device) {
    VkCommandPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].CreateCommandPool(device, &poolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo cmdBufAllocateInfo = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=commandPool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};

    VK_CHECK_RESULT(device_dispatch[GetKey(device)].AllocateCommandBuffers(device, &cmdBufAllocateInfo, &cmd));

    VkCommandBufferBeginInfo cmdBufInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].BeginCommandBuffer(cmd, &cmdBufInfo));
  }
  ~CommandBuffer(){
    device_dispatch[GetKey(device)].FreeCommandBuffers(device, commandPool, 1, &cmd);
  }
  void insertImageMemoryBarrier(
			      VkImage image,
			      VkAccessFlags srcAccessMask,
			      VkAccessFlags dstAccessMask,
			      VkImageLayout oldImageLayout,
			      VkImageLayout newImageLayout,
			      VkPipelineStageFlags srcStageMask,
			      VkPipelineStageFlags dstStageMask,
			      VkImageSubresourceRange subresourceRange) {
    VkImageMemoryBarrier imageMemoryBarrier{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageMemoryBarrier.srcAccessMask = srcAccessMask;
    imageMemoryBarrier.dstAccessMask = dstAccessMask;
    imageMemoryBarrier.oldLayout = oldImageLayout;
    imageMemoryBarrier.newLayout = newImageLayout;
    imageMemoryBarrier.image = image;
    imageMemoryBarrier.subresourceRange = subresourceRange;

    device_dispatch[GetKey(device)].CmdPipelineBarrier(
			 cmd,
			 srcStageMask,
			 dstStageMask,
			 0,
			 0, nullptr,
			 0, nullptr,
			 1, &imageMemoryBarrier);
  }
  void copyImage(VkImage src, VkImage dst, VkExtent2D imgSize){
    VkImageCopy imageCopyRegion{};
    imageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopyRegion.srcSubresource.layerCount = 1;
    imageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopyRegion.dstSubresource.layerCount = 1;
    imageCopyRegion.extent.width = imgSize.width;
    imageCopyRegion.extent.height = imgSize.height;
    imageCopyRegion.extent.depth = 1;

    // Issue the copy command
    device_dispatch[GetKey(device)].CmdCopyImage(
		   cmd,
		   src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		   1,
		   &imageCopyRegion);
  }
  void end(){
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].EndCommandBuffer(cmd));
  }
  void submit(VkQueue queue, VkFence fence, std::vector<VkSemaphore> semaphores){
    VkSubmitInfo submitInfo = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.waitSemaphoreCount = semaphores.size();
    submitInfo.pWaitSemaphores = semaphores.data();

    // Submit to the queue
    VK_CHECK_RESULT(device_dispatch[GetKey(queue)].QueueSubmit(queue, 1, &submitInfo, fence));
  }
};


CreateOtherDevice *cod;
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{
  TRACE("in function: creating device");
  {
    auto info = pCreateInfo;
    while(info != nullptr){
      TRACE("Extension: " << info->sType);
      info = (VkDeviceCreateInfo *) info->pNext;
    }
  }
  VkLayerDeviceCreateInfo *layerCreateInfo = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  // store info for subsequent create call
  const auto targetLayerInfo = layerCreateInfo->u.pLayerInfo;
  VkDevice pDeviceLogic = *pDevice;

  PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
  VkResult ret = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);
  {
    scoped_lock l(global_lock);
    TRACE("spawning secondary device creation");
    static bool first = true;
    if(first){
      first = false;
      // hopefully the first createFunc has only modified this one field
      layerCreateInfo->u.pLayerInfo = targetLayerInfo;
      TRACE("After reset:" << layerCreateInfo->u.pLayerInfo);
      auto display_dev = instance_info[GetKey(physicalDevice)].display;
      // VkDeviceCreateInfo displayCreate{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      //VkDevice display_dev = {};
      // VK_CHECK_RESULT(dispatchTable.CreateDevice(display, &displayCreate, nullptr, &display_dev));



      cod = new CreateOtherDevice{display_dev, physicalDevice, *pDevice};
      cod->start();
      pthread_yield();
    }
  }

  // store the table by key
  {
    scoped_lock l(global_lock);
    device_dispatch[GetKey(*pDevice)] = fetchDispatchTable(gdpa, pDevice);
  }
  TRACE("CreateDevice done");

  return VK_SUCCESS;

}

VkLayerDispatchTable fetchDispatchTable(PFN_vkGetDeviceProcAddr gdpa, VkDevice *pDevice){
  TRACE("fetching dispatch for " << GetKey(*pDevice));
  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerDispatchTable dispatchTable;
#define FETCH(x) dispatchTable.x = (PFN_vk##x)gdpa(*pDevice, "vk" #x);
#define _FETCH(x) dispatchTable.x =(PFN_vk##x) dlsym(libvulkan.get(), "vk" #x);
  FETCH(GetDeviceProcAddr);
  TRACE("GetDeviceProcAddr is: " << (void*) dispatchTable.GetDeviceProcAddr);
  FETCH(DestroyDevice);
  FETCH(BeginCommandBuffer);
  FETCH(CmdDraw);
  FETCH(CmdDrawIndexed);
  FETCH(EndCommandBuffer);

  FETCH(CreateSwapchainKHR);
  TRACE("Create Swapchain KHR is: " << (void*) dispatchTable.CreateSwapchainKHR);
  FETCH(DestroySwapchainKHR);
  FETCH(GetSwapchainImagesKHR);
  FETCH(AcquireNextImageKHR);
  FETCH(QueuePresentKHR);

  FETCH(CreateImage);
  FETCH(GetImageMemoryRequirements);
  FETCH(AllocateMemory);
  FETCH(BindImageMemory);
  FETCH(GetImageSubresourceLayout);
  FETCH(FreeMemory);
  FETCH(DestroyImage);
  FETCH(MapMemory);
  FETCH(UnmapMemory);


  _FETCH(AllocateCommandBuffers);
  FETCH(BeginCommandBuffer);
  FETCH(CmdCopyImage);
  FETCH(CmdPipelineBarrier);
  FETCH(CreateCommandPool);
  //FETCH(CreateDevice);
  FETCH(EndCommandBuffer);
  //FETCH(EnumeratePhysicalDevices);
  FETCH(FreeCommandBuffers);
  //FETCH(GetPhysicalDeviceMemoryProperties);
  //FETCH(GetPhysicalDeviceQueueFamilyProperties);
  _FETCH(QueueSubmit);

  _FETCH(GetDeviceQueue);

  FETCH(CreateFence);
  FETCH(WaitForFences);
  FETCH(DestroyFence);

  FETCH(CreateSemaphore);
  FETCH(DestroySemaphore);

#undef FETCH
  return dispatchTable;
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  device_dispatch.erase(GetKey(device));
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
  {
    scoped_lock l(global_lock);
    if(cod == nullptr){
      std::cerr << "no thread to join\n";
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    TRACE("joining secondary device creation");
    TRACE("When startup hangs here, you have probably hit the initialization deadlock");
    cod->join();
    TRACE("joining succeeded. Luckily initialization deadlock did not occur.");
  }
  VkDevice render_gpu = device;
  VkSwapchainCreateInfoKHR info2 = *pCreateInfo;
  info2.minImageCount = 3;
  pCreateInfo = &info2;
  VkSwapchainKHR old = pCreateInfo->oldSwapchain;
  if(old != VK_NULL_HANDLE){
    MySwapchain *ch = reinterpret_cast<MySwapchain*>(old);
    info2.oldSwapchain = ch->backend;
    TRACE("Old Swapchain: " << ch->backend);
  }
  TRACE("Creating Swapchain for size: " << pCreateInfo->imageExtent.width << "x" << pCreateInfo->imageExtent.height);
  TRACE("MinImageCount: " << pCreateInfo->minImageCount);
  TRACE("fetching device for: " << GetKey(render_gpu));
  VkDevice display_gpu = cod->display_gpu;
  TRACE("found: " << GetKey(display_gpu));

  TRACE("FamilyIndexCount: " <<  pCreateInfo->queueFamilyIndexCount);
  TRACE("Dev: " << GetKey(display_gpu));
  TRACE("Swapchainfunc: " << (void*) device_dispatch[GetKey(display_gpu)].CreateSwapchainKHR);

  MySwapchain *ch = new MySwapchain();
  ch->display_device = display_gpu;
  // TODO automatically find correct queue and not choose 0 forcibly
  device_dispatch[GetKey(render_gpu)].GetDeviceQueue(render_gpu, 0, 0, &ch->render_queue);
  device_dispatch[GetKey(display_gpu)].GetDeviceQueue(display_gpu, 0, 0, &ch->display_queue);
  ch->device = render_gpu;
  ch->render_images.resize(pCreateInfo->minImageCount);
  ch->render_copy_images.resize(pCreateInfo->minImageCount);
  ch->display_src_images.resize(pCreateInfo->minImageCount);
  ch->display_commands.resize(pCreateInfo->minImageCount);
  ch->imgSize = pCreateInfo->imageExtent;
  ch->sem = std::move(std::unique_ptr<Semaphore>(new Semaphore(display_gpu)));

  VkMemoryPropertyFlags host_mem = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryPropertyFlags local_mem = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  ssize_t render_host_mem = -1;
  ssize_t render_local_mem = -1;
  ssize_t display_host_mem = -1;
  for(size_t j=0; j < cod->render_mem.memoryTypeCount; j++){
    if ( render_host_mem == -1 && ( cod->render_mem.memoryTypes[j].propertyFlags & host_mem ) == host_mem ) {
      render_host_mem = j;
    }
    if ( render_local_mem == -1 && ( cod->render_mem.memoryTypes[j].propertyFlags & local_mem ) == local_mem ) {
      render_local_mem = j;
    }
  }
  for(size_t j=0; j < cod->display_mem.memoryTypeCount; j++){
    if ( display_host_mem == -1 && ( cod->display_mem.memoryTypes[j].propertyFlags & host_mem ) == host_mem ) {
      display_host_mem = j;
    }
  }
  TRACE("Selected render mem: " << render_host_mem << ";" << render_local_mem << " display: " << display_host_mem);
  size_t i = 0;
  for( auto &renderImage: ch->render_images){
    renderImage = std::make_shared<FramebufferImage>(render_gpu, pCreateInfo->imageExtent,
        VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |/**/ VK_IMAGE_USAGE_TRANSFER_SRC_BIT, render_local_mem);
    auto &renderCopyImage = ch->render_copy_images[i];
    renderCopyImage = std::make_shared<FramebufferImage>(render_gpu, pCreateInfo->imageExtent,
	VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT, render_host_mem);
    auto &displaySrcImage = ch->display_src_images[i++];
    displaySrcImage = std::make_shared<FramebufferImage>(display_gpu, pCreateInfo->imageExtent,
	VK_IMAGE_TILING_LINEAR,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |/**/ VK_IMAGE_USAGE_TRANSFER_SRC_BIT, display_host_mem);

    renderCopyImage->map();
    displaySrcImage->map();

    CommandBuffer cmd{ch->display_device};
    cmd.insertImageMemoryBarrier(
			   displaySrcImage->img,
			   0,
			   VK_ACCESS_MEMORY_WRITE_BIT,
			   VK_IMAGE_LAYOUT_UNDEFINED,
			   VK_IMAGE_LAYOUT_GENERAL,
			   VK_PIPELINE_STAGE_TRANSFER_BIT,
			   VK_PIPELINE_STAGE_TRANSFER_BIT,
			   VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.end();
    Fence f{ch->display_device};
    cmd.submit(ch->display_queue, f.fence, {});
    f.await();
  }
  *pSwapchain = reinterpret_cast<VkSwapchainKHR>(ch);


  VkResult rc = device_dispatch[GetKey(display_gpu)].CreateSwapchainKHR(display_gpu, pCreateInfo, pAllocator, &ch->backend);
  TRACE(">> Swapchain create done " << rc << ";" << (void*) ch->backend);

  uint32_t count;
  device_dispatch[GetKey(ch->display_device)].GetSwapchainImagesKHR(ch->display_device, ch->backend, &count, nullptr);
  TRACE("Image aquiring: " << count << "; created: " << i);
  ch->display_images.resize(count);
  device_dispatch[GetKey(ch->display_device)].GetSwapchainImagesKHR(ch->display_device, ch->backend, &count, ch->display_images.data());
  return rc;
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    if(swapchain == nullptr) { return;}
  MySwapchain *ch = reinterpret_cast<MySwapchain*>(swapchain);
  TRACE(">> Destroy swapchain: " << (void*) ch->backend);
  // TODO: the Nvidia driver segfaults when passing a chain here?
  // device_dispatch[GetKey(device)].DestroySwapchainKHR(device, ch->backend, pAllocator);
  ch->stop();
  delete ch;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages) {
  MySwapchain *ch = reinterpret_cast<MySwapchain*>(swapchain);

  *pSwapchainImageCount = ch->render_images.size();
  VkResult res = VK_SUCCESS;
  if(pSwapchainImages != nullptr) {
    TRACE("Get Swapchain Images buffer: " <<  pSwapchainImages);
    res = VK_SUCCESS; //device_dispatch[GetKey(device)].GetSwapchainImagesKHR(device, ch->backend, pSwapchainImageCount, pSwapchainImages);
    for(size_t i = 0; i < *pSwapchainImageCount; i++){
      pSwapchainImages[i] = ch->render_images[i]->img;
    }
    TRACE("Count: " << *pSwapchainImageCount);
  }
  return res;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
  TRACE_FRAME("AcquireNextImage: sem: " << semaphore << ", fence: " << fence);
  MySwapchain *ch = reinterpret_cast<MySwapchain*>(swapchain);

  VkResult res;
  {
    Fence myfence{ch->display_device};

    res = device_dispatch[GetKey(ch->display_device)].AcquireNextImageKHR(ch->display_device, ch->backend, timeout, nullptr, myfence.fence, pImageIndex);
    TRACE_FRAME("AcquireNextImageKHR: " << *pImageIndex << ";" << res);

    myfence.await();
  }
  VkSubmitInfo qsi{};
  qsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  qsi.signalSemaphoreCount = 1;
  qsi.pSignalSemaphores = &semaphore;
  device_dispatch[GetKey(ch->render_queue)].QueueSubmit(ch->render_queue, 1, &qsi, nullptr);
  return res;
}
#include <iostream>
#include <string>
#include <fstream>
#include <algorithm>

void MySwapchain::storeImage(uint32_t index, VkDevice device, VkExtent2D imgSize, VkQueue queue, VkFormat colorFormat, std::vector<VkSemaphore> wait_on, Fence &notify){
  auto cpyImage = render_copy_images[index];
  auto srcImage = render_images[index]->img;
  CommandBuffer cmd{device};
    cmd.insertImageMemoryBarrier(
	cpyImage->img,
	0,					VK_ACCESS_TRANSFER_WRITE_BIT,
	VK_IMAGE_LAYOUT_UNDEFINED,		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.insertImageMemoryBarrier(
	srcImage,
	VK_ACCESS_MEMORY_READ_BIT,		VK_ACCESS_TRANSFER_READ_BIT,
	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

    cmd.copyImage(srcImage, cpyImage->img, imgSize);

    cmd.insertImageMemoryBarrier(
	cpyImage->img,
	VK_ACCESS_TRANSFER_WRITE_BIT,		VK_ACCESS_MEMORY_READ_BIT,
	VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,	VK_IMAGE_LAYOUT_GENERAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.insertImageMemoryBarrier(
	srcImage,
	VK_ACCESS_TRANSFER_READ_BIT,		VK_ACCESS_MEMORY_READ_BIT,
	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

    cmd.end();
    cmd.submit(queue, notify.fence, wait_on);
}
#include <chrono>

void MySwapchain::copyImageData(uint32_t index, std::vector<VkSemaphore> sems){
  {
    std::unique_lock<std::mutex> lock(queueMutex);
    has_work.notify_all();
  }

  {
    auto rendered = render_copy_images[index]->getMapped();
    auto display = display_src_images[index]->getMapped();
    auto rendered_layout = render_copy_images[index]->getLayout();
    auto display_layout = display_src_images[index]->getLayout();
    auto rendered_start = rendered->data + rendered_layout.offset;
    auto display_start = display->data + display_layout.offset;
    if(rendered_layout.size/rendered_layout.rowPitch != display_layout.size/display_layout.rowPitch){
      TRACE("Layouts don't match at all");
      throw std::runtime_error("Layouts don't match at all");
    }

    auto start = std::chrono::steady_clock::now();
    if(rendered_layout.rowPitch == display_layout.rowPitch){
      memcpy(display_start, rendered_start, rendered_layout.size);
    }else{
      VkDeviceSize display_offset = 0;
      VkDeviceSize minRowPitch = rendered_layout.rowPitch;
      if(display_layout.rowPitch < minRowPitch){
	minRowPitch = display_layout.rowPitch;
      }
      for(VkDeviceSize offset = 0; offset < rendered_layout.size; offset += rendered_layout.rowPitch){
	memcpy(display_start + display_offset, rendered_start + offset, minRowPitch);
	display_offset += display_layout.rowPitch;
      }
    }
    TRACE_PROFILING("Time for plain memcpy: " << std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count() << " seconds");
  }

  if(display_commands[index] == nullptr){
    display_commands[index] = std::make_shared<CommandBuffer>(display_device);
    CommandBuffer &cmd = *display_commands[index];
  cmd.insertImageMemoryBarrier(
	display_src_images[index]->img,
	VK_ACCESS_MEMORY_WRITE_BIT,	VK_ACCESS_TRANSFER_READ_BIT,
	VK_IMAGE_LAYOUT_GENERAL,	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	 VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
  cmd.insertImageMemoryBarrier(
	display_images[index],
	VK_ACCESS_MEMORY_READ_BIT,	VK_ACCESS_TRANSFER_WRITE_BIT,
	VK_IMAGE_LAYOUT_UNDEFINED,	VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
  cmd.copyImage(display_src_images[index]->img, display_images[index], imgSize);

  cmd.insertImageMemoryBarrier(
	display_src_images[index]->img,
	VK_ACCESS_TRANSFER_READ_BIT,	VK_ACCESS_MEMORY_WRITE_BIT,
	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,	VK_IMAGE_LAYOUT_GENERAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
  cmd.insertImageMemoryBarrier(
	display_images[index],
	VK_ACCESS_TRANSFER_READ_BIT,	VK_ACCESS_MEMORY_READ_BIT,
	VK_IMAGE_LAYOUT_UNDEFINED,	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

  cmd.end();

  }

  display_commands[index]->submit(display_queue, nullptr, sems);
}

void MySwapchain::queue(VkQueue queue, const VkPresentInfoKHR* pPresentInfo){
  std::unique_lock<std::mutex> lock(queueMutex);

  auto workItem = QueueItem{queue, *pPresentInfo, pPresentInfo->pImageIndices[0], std::unique_ptr<Fence>{new Fence{device}}};
  storeImage(workItem.imgIndex, device, imgSize, render_queue, VK_FORMAT_B8G8R8A8_UNORM, std::vector<VkSemaphore>{pPresentInfo->pWaitSemaphores, pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount}, *workItem.fence);

  work.push_back(std::move(workItem));
  has_work.notify_all();
}
void MySwapchain::stop(){
  {
    std::unique_lock<std::mutex> lock(queueMutex);
    active = false;
    has_work.notify_all();
  }
  myThread->join();
  myThread.reset();
}
void MySwapchain::present(const QueueItem &workItem){
    auto &queue = workItem.queue;
    const auto pPresentInfo = &workItem.pPresentInfo;
    const auto index = workItem.imgIndex;

    const auto start = std::chrono::steady_clock::now();
    copyImageData(index, {sem->sem});

    TRACE_FRAME("Swapchain QueuePresent: #semaphores: " << pPresentInfo->waitSemaphoreCount << ", #chains: " << pPresentInfo->swapchainCount << ", imageIndex: " << index);
    TRACE_PROFILING("Own time for present: " << std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start).count() << " seconds");

    VkPresentInfoKHR p2 = {.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    p2.pSwapchains = &backend;
    p2.swapchainCount = 1;
    p2.pWaitSemaphores = &sem->sem;
    p2.waitSemaphoreCount = 1;
    p2.pImageIndices = &index;

    VkResult res = device_dispatch[GetKey(display_queue)].QueuePresentKHR(display_queue, &p2);
    if(res != VK_SUCCESS) {
      TRACE("ERROR, Queue Present failed\n");
    }
}
void MySwapchain::run(){
  while(true){
    QueueItem workItem{};
    {
      std::unique_lock<std::mutex> lock(queueMutex);
      has_work.wait(lock, [this](){return !active || work.size() > 0;});
      if(!active) return;
      workItem = std::move(work.front());
      work.pop_front();
    }
    present(workItem);
  }
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
  const auto start = std::chrono::steady_clock::now();
  if(pPresentInfo->swapchainCount != 1){
    TRACE("Warning, presenting with multiple swapchains not implemented, ignoring");
  }

  MySwapchain *ch = reinterpret_cast<MySwapchain*>(pPresentInfo->pSwapchains[0]);
  double secs = std::chrono::duration_cast<std::chrono::duration<double>>(start - ch->lastPresent).count();
  TRACE_PROFILING("Time between VkQueuePresents: " << secs << " -> " << 1/secs << " FPS");
  ch->lastPresent = start;

  ch->queue(queue, pPresentInfo);

  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
  TRACE("Fetching function...");
  PFN_vkCreateXcbSurfaceKHR fn = (PFN_vkCreateXcbSurfaceKHR)instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR");
  TRACE("Xcb create surface: " << fn);
  return fn(instance, pCreateInfo, pAllocator, pSurface);
}


VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceFormatsKHR(phy, surface, pSurfaceFormatCount, pSurfaceFormats);
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties* pQueueFamilyProperties) {
  VkPhysicalDevice phy = physicalDevice;
  instance_dispatch[GetKey(phy)].GetPhysicalDeviceQueueFamilyProperties(phy, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  return instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceCapabilitiesKHR(phy, surface, pSurfaceCapabilities);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  queueFamilyIndex = 0;
  auto res = instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfaceSupportKHR(phy, queueFamilyIndex, surface, pSupported);
  TRACE("Support: " << GetKey(phy) << ", " << *pSupported);
  return res;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes) {
  VkPhysicalDevice phy = instance_info[GetKey(physicalDevice)].display;
  auto res = instance_dispatch[GetKey(phy)].GetPhysicalDeviceSurfacePresentModesKHR(phy, surface, pPresentModeCount, pPresentModes);
  return res;
}


///////////////////////////////////////////////////////////////////////////////////////////
// Enumeration function

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                                                       VkLayerProperties *pProperties)
{
  if(pPropertyCount) *pPropertyCount = 1;

  if(pProperties)
  {
    strcpy(pProperties->layerName, "VK_LAYER_PRIMUS_PrimusVK");
    strcpy(pProperties->description, "Primus-vk - https://github.com/felixdoerre/primus_vk");
    pProperties->implementationVersion = 1;
    pProperties->specVersion = VK_API_VERSION_1_0;
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pPropertyCount, VkLayerProperties *pProperties)
{
  return PrimusVK_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
  if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_PRIMUS_PrimusVK"))
    return VK_ERROR_LAYER_NOT_PRESENT;

  // don't expose any extensions
  if(pPropertyCount) *pPropertyCount = 0;
  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumerateDeviceExtensionProperties(
                                     VkPhysicalDevice physicalDevice, const char *pLayerName,
                                     uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
  // pass through any queries that aren't to us
  if(pLayerName == NULL || strcmp(pLayerName, "VK_LAYER_PRIMUS_PrimusVK"))
  {
    if(physicalDevice == VK_NULL_HANDLE)
      return VK_SUCCESS;

    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(physicalDevice)].EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
  }

  // don't expose any extensions
  if(pPropertyCount) *pPropertyCount = 0;
  return VK_SUCCESS;
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumeratePhysicalDevices(
    VkInstance                                  instance,
    uint32_t*                                   pPhysicalDeviceCount,
    VkPhysicalDevice*                           pPhysicalDevices){
  InstanceInfo &info = instance_info[GetKey(instance)];
  int cnt = 1;
  if(list_all_gpus) cnt = 2;
  if(pPhysicalDevices == nullptr){
    *pPhysicalDeviceCount = cnt;
    return VK_SUCCESS;
  }
  scoped_lock l(global_lock);
  VkResult res = instance_dispatch[GetKey(instance)].EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, nullptr);
  if(res != VK_SUCCESS) return res;
  std::vector<VkPhysicalDevice> vec{*pPhysicalDeviceCount};
  res = instance_dispatch[GetKey(instance)].EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, vec.data());
  if(res != VK_SUCCESS) return res;
  pPhysicalDevices[0] = info.render;
  *pPhysicalDeviceCount = cnt;
  if(cnt >= 2){
    pPhysicalDevices[1] = info.display;
  }
  return res;
}


///////////////////////////////////////////////////////////////////////////////////////////
// GetProcAddr functions, entry points of the layer

#define GETPROCADDR(func) if(!strcmp(pName, "vk" #func)) return (PFN_vkVoidFunction)&PrimusVK_##func;

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL PrimusVK_GetDeviceProcAddr(VkDevice device, const char *pName)
{
  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);

  GETPROCADDR(CreateSwapchainKHR);
  GETPROCADDR(DestroySwapchainKHR);
  GETPROCADDR(GetSwapchainImagesKHR);
  GETPROCADDR(AcquireNextImageKHR);
  GETPROCADDR(QueuePresentKHR);
  GETPROCADDR(GetPhysicalDeviceSurfaceFormatsKHR);
  GETPROCADDR(GetPhysicalDeviceQueueFamilyProperties);
  GETPROCADDR(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  GETPROCADDR(GetPhysicalDeviceSurfaceSupportKHR);
  GETPROCADDR(GetPhysicalDeviceSurfacePresentModesKHR);
  GETPROCADDR(CreateXcbSurfaceKHR);

  {
    scoped_lock l(global_lock);
    return device_dispatch[GetKey(device)].GetDeviceProcAddr(device, pName);
  }
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL PrimusVK_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
  // instance chain functions we intercept
  GETPROCADDR(GetInstanceProcAddr);
  GETPROCADDR(EnumeratePhysicalDevices);
  GETPROCADDR(EnumerateInstanceLayerProperties);
  GETPROCADDR(EnumerateInstanceExtensionProperties);
  GETPROCADDR(CreateInstance);
  GETPROCADDR(DestroyInstance);

  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(EnumerateDeviceLayerProperties);
  GETPROCADDR(EnumerateDeviceExtensionProperties);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);

  GETPROCADDR(CreateSwapchainKHR);
  GETPROCADDR(DestroySwapchainKHR);
  GETPROCADDR(GetSwapchainImagesKHR);
  GETPROCADDR(AcquireNextImageKHR);
  GETPROCADDR(QueuePresentKHR);
  GETPROCADDR(GetPhysicalDeviceSurfaceFormatsKHR);
  GETPROCADDR(GetPhysicalDeviceQueueFamilyProperties);
  GETPROCADDR(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  GETPROCADDR(GetPhysicalDeviceSurfaceSupportKHR);
  GETPROCADDR(GetPhysicalDeviceSurfacePresentModesKHR);
  // GETPROCADDR(CreateXcbSurfaceKHR);

  {
    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, pName);
  }
}
