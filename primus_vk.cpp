#include "vulkan.h"
#include "vk_layer.h"
#include "vk_layer_dispatch_table.h"

#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>

#include <cassert>
#include <cstring>

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
#include <algorithm>
#include <sstream>
#include <string>
#include <chrono>

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

class CreateOtherDevice;

struct InstanceInfo {
  VkInstance instance;
  VkPhysicalDevice render;
  VkPhysicalDevice display;

  CreateOtherDevice *cod;

  bool secondarySpawned;
  std::shared_ptr<std::mutex> renderQueueMutex;
};

std::map<void *, VkLayerInstanceDispatchTable> instance_dispatch;
VkLayerInstanceDispatchTable loader_dispatch;
// VkInstance->disp is beeing malloc'ed for every new instance
// so we can assume it to be a good key.
std::map<void *, InstanceInfo> instance_info;

std::map<void *, InstanceInfo*> device_instance_info;
std::map<void *, VkLayerDispatchTable> device_dispatch;

std::shared_ptr<void> libvulkan(dlopen("libvulkan.so.1", RTLD_NOW), dlclose);


// #define TRACE(x)
#define TRACE(x) std::cout << "PrimusVK: " << x << "\n";
#define TRACE_PROFILING(x)
// #define TRACE_PROFILING(x) std::cout << "PrimusVK: " << x << "\n";
#define TRACE_PROFILING_EVENT(x, y)
// #define TRACE_PROFILING_EVENT(idx, evt) std::cout << "PrimusVK-profiling: " << idx << " " << std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - primus_start).count() << " " << evt << "\n";
#define TRACE_FRAME(x)
// #define TRACE_FRAME(x) std::cout << "PrimusVK: " << x << "\n";

#define VK_CHECK_RESULT(x) do{ const VkResult r = x; if(r != VK_SUCCESS){printf("PrimusVK: Error %d in line %d.\n", r, __LINE__);}}while(0);
// #define VK_CHECK_RESULT(x) if(x != VK_SUCCESS){printf("Error %d, in %d\n", x, __LINE__);}


void GetEnvVendorDeviceIDs(std::string env, uint32_t &vendor, uint32_t &device)
{
  char *envstr = getenv(env.c_str());
  if(envstr != nullptr){
    std::stringstream ss(envstr);
    std::string item;
    std::vector<uint32_t> hexnums(2);
    int i = 0;
    while(std::getline(ss, item, ':') && (i < 2)) {
      uint32_t num = 0;
      std::stringstream _ss;
      _ss << std::hex << item;
      _ss >> num;
      hexnums[i] = num;
      ++i;
    }
    vendor = hexnums[0];
    device = hexnums[1];
  }
}

bool IsDevice(
  VkPhysicalDeviceProperties props, 
  uint32_t vendor, 
  uint32_t device, 
  VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
{
  if((vendor == 0) && (props.deviceType == type)){
    if(type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU){
      TRACE("Got integrated gpu!");
    } else {
      TRACE("Got discrete gpu!");
    }
    TRACE("Device: " << props.deviceName);
    TRACE("  Type: " << props.deviceType);
    return true;
  }
  if((props.vendorID == vendor) && (props.deviceID == device)){
    TRACE("Got device from env!");
    TRACE("Device: " << props.deviceName);
    TRACE("  Type: " << props.deviceType);
    return true;
  }
  if(props.vendorID == vendor){
    TRACE("Got device from env! (via vendorID)");
    TRACE("Device: " << props.deviceName);
    TRACE("  Type: " << props.deviceType);
    return true;
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////////////////
// Layer init and shutdown
VkLayerDispatchTable fetchDispatchTable(PFN_vkGetDeviceProcAddr gdpa, VkDevice *pDevice);
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
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

  uint32_t displayVendorID = 0;
  uint32_t displayDeviceID = 0;
  uint32_t renderVendorID = 0;
  uint32_t renderDeviceID = 0;
  GetEnvVendorDeviceIDs("PRIMUS_VK_DISPLAYID", displayVendorID, displayDeviceID);
  GetEnvVendorDeviceIDs("PRIMUS_VK_RENDERID", renderVendorID, renderDeviceID);

  std::vector<VkPhysicalDevice> physicalDevices;
  {
    auto enumerateDevices = dispatchTable.EnumeratePhysicalDevices;
    TRACE("Getting devices");
    uint32_t gpuCount = 0;
    enumerateDevices(*pInstance, &gpuCount, nullptr);
    physicalDevices.resize(gpuCount);
    enumerateDevices(*pInstance, &gpuCount, physicalDevices.data());
  }

  TRACE("Searching for display GPU:");
  VkPhysicalDevice display = VK_NULL_HANDLE;
  for(auto &dev: physicalDevices){
    VkPhysicalDeviceProperties props;
    dispatchTable.GetPhysicalDeviceProperties(dev, &props);
    TRACE(dev << ": ");
    if(IsDevice(props, displayVendorID, displayDeviceID, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)){
      display = dev;
      break;
    }
  }

  TRACE("Searching for render GPU:");
  VkPhysicalDevice render = VK_NULL_HANDLE;
  for(auto &dev: physicalDevices){
    VkPhysicalDeviceProperties props;
    dispatchTable.GetPhysicalDeviceProperties(dev, &props);
    TRACE(dev << ".");
    if(IsDevice(props, renderVendorID, renderDeviceID)){
      render = dev;
      break;
    }
  }
  if(display == VK_NULL_HANDLE || render == VK_NULL_HANDLE){
    const auto c_icd_filenames = getenv("VK_ICD_FILENAMES");
    if(display == VK_NULL_HANDLE) {
      TRACE("No device for the display GPU found. Are the intel-mesa drivers installed?");
    }
    if(render == VK_NULL_HANDLE) {
      TRACE("No device for the rendering GPU found. Is the correct driver installed?");
    }
    if(c_icd_filenames != nullptr) {
      TRACE("VK_ICD_FILENAMES=" << c_icd_filenames);
    } else {
      TRACE("VK_ICD_FILENAMES not set");
    }
    return VK_ERROR_INITIALIZATION_FAILED;
  }
#define FORWARD(func) dispatchTable.func = (PFN_vk##func)gpa(*pInstance, "vk" #func);
  FORWARD(GetPhysicalDeviceQueueFamilyProperties);
#include "primus_vk_forwarding.h"
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
    instance_info[GetKey(*pInstance)] = InstanceInfo{.instance = *pInstance, .render = render, .display=display, .cod=nullptr, .secondarySpawned = false, .renderQueueMutex = std::make_shared<std::mutex>()};
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);

  instance_dispatch[GetKey(instance)].DestroyInstance(instance, pAllocator);

  instance_dispatch.erase(GetKey(instance));
  instance_info.erase(GetKey(instance));
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
  FramebufferImage(FramebufferImage &) = delete;
  FramebufferImage(VkDevice device, VkExtent2D size, VkImageTiling tiling, VkImageUsageFlags usage, VkFormat format, int memoryTypeIndex): device(device){
    TRACE("Creating image: " << size.width << "x" << size.height);
    VkImageCreateInfo imageCreateCI {};
    imageCreateCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
    imageCreateCI.format = format;
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
    mapped.reset();
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
    VkFenceCreateInfo fenceInfo = {.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = 0;
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].CreateFence(device, &fenceInfo, nullptr, &fence));
  }
  void await(){
    // Wait for the fence to signal that command buffer has finished executing
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].WaitForFences(device, 1, &fence, VK_TRUE, 10000000000L));
  }
  void reset(){
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].ResetFences(device, 1, &fence));
  }
  Fence(Fence &&other): device(other.device), fence(other.fence){
    other.fence = VK_NULL_HANDLE;
  }
  ~Fence(){
    if(fence != VK_NULL_HANDLE){
      device_dispatch[GetKey(device)].DestroyFence(device, fence, nullptr);
    }
  }
};
class Semaphore{
  VkDevice device;
public:
  VkSemaphore sem;
  Semaphore(VkDevice dev): device(dev){
    VkSemaphoreCreateInfo semInfo = {.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semInfo.flags = 0;
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].CreateSemaphore(device, &semInfo, nullptr, &sem));
  }
  Semaphore(Semaphore &&other): device(other.device), sem(other.sem) {
    other.sem = VK_NULL_HANDLE;
    other.device = VK_NULL_HANDLE;
  }
  ~Semaphore(){
    if(sem != VK_NULL_HANDLE){
      device_dispatch[GetKey(device)].DestroySemaphore(device, sem, nullptr);
    }
  }
};
struct PrimusSwapchain;
struct ImageWorker {
  PrimusSwapchain &swapchain;

  std::shared_ptr<FramebufferImage> render_image;
  std::shared_ptr<FramebufferImage> render_copy_image;
  std::shared_ptr<FramebufferImage> display_src_image;
  Fence render_copy_fence;
  Semaphore display_semaphore;
  VkImage display_image = VK_NULL_HANDLE;

  std::shared_ptr<CommandBuffer> render_copy_command;
  std::shared_ptr<CommandBuffer> display_command;
  std::unique_ptr<Fence> display_command_fence;

  ImageWorker(PrimusSwapchain &swapchain, VkImage display_image, const VkSwapchainCreateInfoKHR &createInfo, std::tuple<ssize_t, ssize_t, ssize_t> image_memory_types);
  ImageWorker(ImageWorker &&other) = default;
  ~ImageWorker();
  void initImages( std::tuple<ssize_t, ssize_t, ssize_t> image_memory_types, const VkSwapchainCreateInfoKHR &createInfo);
  void createCommandBuffers();
  void copyImageData(std::vector<VkSemaphore> sems);
};
struct PrimusSwapchain{
  std::chrono::steady_clock::time_point lastPresent = std::chrono::steady_clock::now();
  VkDevice device;
  VkQueue render_queue;
  VkDevice display_device;
  std::mutex displayQueueMutex;
  VkQueue display_queue;
  VkSwapchainKHR backend;
  std::vector<ImageWorker> images;
  VkExtent2D imgSize;

  std::vector<std::unique_ptr<std::thread>> threads;

  CreateOtherDevice *cod;
  PrimusSwapchain(PrimusSwapchain &) = delete;
  PrimusSwapchain(VkDevice device, VkDevice display_device, VkSwapchainKHR backend, const VkSwapchainCreateInfoKHR *pCreateInfo, uint32_t imageCount, CreateOtherDevice *cod):
    device(device), display_device(display_device), backend(backend), cod(cod){
    // TODO automatically find correct queue and not choose 0 forcibly
    device_dispatch[GetKey(device)].GetDeviceQueue(device, 0, 0, &render_queue);
    device_dispatch[GetKey(display_device)].GetDeviceQueue(display_device, 0, 0, &display_queue);

    uint32_t image_count;
    device_dispatch[GetKey(display_device)].GetSwapchainImagesKHR(display_device, backend, &image_count, nullptr);
    TRACE("Image aquiring: " << image_count);
    std::vector<VkImage> display_images;
    display_images.resize(image_count);
    device_dispatch[GetKey(display_device)].GetSwapchainImagesKHR(display_device, backend, &image_count, display_images.data());

    imgSize = pCreateInfo->imageExtent;

    auto image_memory_types = getImageMemories();
    for(uint32_t i = 0; i < imageCount; i++){
      images.emplace_back(*this, display_images[i], *pCreateInfo, image_memory_types);
    }

    TRACE("Creating a Swapchain thread.");
    size_t thread_count = 1;
    char *m_env = getenv("PRIMUS_VK_MULTITHREADING");
    if(m_env == nullptr || std::string{m_env} != "1"){
      thread_count = imageCount;
    }
    threads.resize(thread_count);
    for(auto &thread: threads){
      thread = std::unique_ptr<std::thread>(new std::thread([this](){this->run();}));
      pthread_setname_np(thread->native_handle(), "swapchain-thread");
    }
  }

  std::tuple<ssize_t, ssize_t, ssize_t> getImageMemories();

  void storeImage(uint32_t index, VkQueue queue, std::vector<VkSemaphore> wait_on, Fence &notify);

  void queue(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);

  std::mutex queueMutex;
  std::condition_variable has_work;
  bool active = true;
  struct QueueItem {
    VkQueue queue;
    VkPresentInfoKHR pPresentInfo;
    uint32_t imgIndex;
  };
  std::list<QueueItem> work;
  std::list<QueueItem> in_progress;
  void present(const QueueItem &workItem);
  void run();
  void stop();
};

ImageWorker::ImageWorker(PrimusSwapchain &swapchain, VkImage display_image, const VkSwapchainCreateInfoKHR &createInfo, std::tuple<ssize_t, ssize_t, ssize_t> image_memory_types): swapchain(swapchain), render_copy_fence(swapchain.device), display_semaphore(swapchain.display_device), display_image(display_image){
  initImages(image_memory_types, createInfo);
  createCommandBuffers();
}
ImageWorker::~ImageWorker(){
  if(display_command_fence){
    display_command_fence->await();
  }
}

bool list_all_gpus = false;
class CreateOtherDevice {
public:
  VkPhysicalDevice display_dev;
  VkPhysicalDevice render_dev;
  VkPhysicalDeviceMemoryProperties display_mem;
  VkPhysicalDeviceMemoryProperties render_mem;
  VkDevice render_gpu;
  VkDevice display_gpu;

  std::unique_ptr<std::thread> thread;
  CreateOtherDevice(VkPhysicalDevice display_dev, VkPhysicalDevice render_dev, VkDevice render_gpu):
    display_dev(display_dev), render_dev(render_dev), render_gpu(render_gpu){
  }
  void run(){
    auto &minstance_info = instance_info[GetKey(render_dev)];

    VkDevice pDeviceLogic;
    TRACE("Device creation thread running");
    uint32_t gpuCount;
    list_all_gpus = true;
    loader_dispatch.EnumeratePhysicalDevices(minstance_info.instance, &gpuCount, nullptr);
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
	TRACE(" flags: " << props.queueFlags);
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
    VkResult ret = loader_dispatch.CreateDevice(display_dev, &createInfo, nullptr, &pDeviceLogic);
    TRACE("Create Graphics FINISHED!: " << ret);
    TRACE("Display: " << GetKey(pDeviceLogic));
    TRACE("storing as reference to: " << GetKey(render_gpu));
    display_gpu = pDeviceLogic;

  }

  void start(){
    thread = std::unique_ptr<std::thread>(new std::thread([this](){this->run();}));
  }
  void join(){
    if(thread == nullptr) { TRACE( "Refusing second join" ); return; }
    thread->join();
    thread.reset();
  }
};


class CommandBuffer {
  VkCommandPool commandPool;
  VkDevice device;
public:
  VkCommandBuffer cmd;
  CommandBuffer(VkDevice device) : device(device) {
    VkCommandPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = 0;
    VK_CHECK_RESULT(device_dispatch[GetKey(device)].CreateCommandPool(device, &poolInfo, nullptr, &commandPool));
    VkCommandBufferAllocateInfo cmdBufAllocateInfo = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdBufAllocateInfo.commandPool = commandPool;
    cmdBufAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdBufAllocateInfo.commandBufferCount = 1;

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
  void submit(VkQueue queue, VkFence fence, std::vector<VkSemaphore> wait = {}, std::vector<VkSemaphore> signal = {}){
    VkSubmitInfo submitInfo = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.waitSemaphoreCount = wait.size();
    submitInfo.pWaitSemaphores = wait.data();
    submitInfo.signalSemaphoreCount = signal.size();
    submitInfo.pSignalSemaphores = signal.data();

    // Submit to the queue
    VK_CHECK_RESULT(device_dispatch[GetKey(queue)].QueueSubmit(queue, 1, &submitInfo, fence));
  }
};

void ImageWorker::initImages( std::tuple<ssize_t, ssize_t, ssize_t> image_memory_types, const VkSwapchainCreateInfoKHR &createInfo){
  ssize_t render_local_mem, render_host_mem, display_host_mem;
  std::tie( render_local_mem, render_host_mem, display_host_mem) = image_memory_types;
  auto imgSize = createInfo.imageExtent;
  auto format = createInfo.imageFormat;
    
  auto &renderImage = render_image;
  auto &renderCopyImage = render_copy_image;
  auto &displaySrcImage = display_src_image;
  renderImage = std::make_shared<FramebufferImage>(swapchain.device, imgSize,
						     VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |/**/ VK_IMAGE_USAGE_TRANSFER_SRC_BIT,format, render_local_mem);
  renderCopyImage = std::make_shared<FramebufferImage>(swapchain.device, imgSize,
							 VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_DST_BIT,format, render_host_mem);
  displaySrcImage = std::make_shared<FramebufferImage>(swapchain.display_device, imgSize,
							 VK_IMAGE_TILING_LINEAR,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |/**/ VK_IMAGE_USAGE_TRANSFER_SRC_BIT,format, display_host_mem);

  renderCopyImage->map();
  displaySrcImage->map();

  CommandBuffer cmd{swapchain.display_device};
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
  Fence f{swapchain.display_device};
  cmd.submit(swapchain.display_queue, f.fence);
  f.await();
}



VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_CreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{
  auto &my_instance_info = instance_info[GetKey(physicalDevice)];
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

  PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
  VkResult ret = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);
  {
    static bool first = true;
    if(!my_instance_info.secondarySpawned){
      scoped_lock l(global_lock);
      my_instance_info.secondarySpawned = true;
      TRACE("spawning secondary device creation: " << &my_instance_info);
      // hopefully the first createFunc has only modified this one field
      layerCreateInfo->u.pLayerInfo = targetLayerInfo;
      TRACE("After reset:" << layerCreateInfo->u.pLayerInfo);
      auto display_dev = my_instance_info.display;

      my_instance_info.cod = new CreateOtherDevice{display_dev, physicalDevice, *pDevice};
      my_instance_info.cod->start();
      pthread_yield();
    }
  }

  // store the table by key
  {
    scoped_lock l(global_lock);
    device_instance_info[GetKey(*pDevice)] = &my_instance_info;
    device_dispatch[GetKey(*pDevice)] = fetchDispatchTable(gdpa, pDevice);
  }
  TRACE("CreateDevice done");

  return ret;

}

VkLayerDispatchTable fetchDispatchTable(PFN_vkGetDeviceProcAddr gdpa, VkDevice *pDevice){
  TRACE("fetching dispatch for " << GetKey(*pDevice));
  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerDispatchTable dispatchTable;
#define FETCH(x) dispatchTable.x = (PFN_vk##x)gdpa(*pDevice, "vk" #x);
#define _FETCH(x) dispatchTable.x =(PFN_vk##x) dlsym(libvulkan.get(), "vk" #x);
  FETCH(GetDeviceProcAddr);
  FETCH(DestroyDevice);
  FETCH(BeginCommandBuffer);
  FETCH(CmdDraw);
  FETCH(CmdDrawIndexed);
  FETCH(EndCommandBuffer);

  FETCH(CreateSwapchainKHR);
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
  FETCH(QueueSubmit);
  FETCH(DeviceWaitIdle);
  FETCH(QueueWaitIdle);

  _FETCH(GetDeviceQueue);

  FETCH(CreateFence);
  FETCH(WaitForFences);
  FETCH(ResetFences);
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
  auto &my_instance = *device_instance_info[GetKey(device)];
  {
    if(my_instance.cod == nullptr){
      std::cerr << "no thread to join\n";
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    TRACE("joining secondary device creation");
    TRACE("When startup hangs here, you have probably hit the initialization deadlock");
    my_instance.cod->join();
    TRACE("joining succeeded. Luckily initialization deadlock did not occur.");
  }
  TRACE("Application requested " << pCreateInfo->minImageCount << " images.");
  VkDevice render_gpu = device;
  VkSwapchainCreateInfoKHR info2 = *pCreateInfo;
  info2.minImageCount = std::max(3u, pCreateInfo->minImageCount);
  info2.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  pCreateInfo = &info2;
  
  VkSwapchainKHR old = pCreateInfo->oldSwapchain;
  if(old != VK_NULL_HANDLE){
    PrimusSwapchain *ch = reinterpret_cast<PrimusSwapchain*>(old);
    info2.oldSwapchain = ch->backend;
    TRACE("Old Swapchain: " << ch->backend);
  }
  TRACE("Creating Swapchain for size: " << pCreateInfo->imageExtent.width << "x" << pCreateInfo->imageExtent.height);
  TRACE("MinImageCount: " << pCreateInfo->minImageCount);
  TRACE("fetching device for: " << GetKey(render_gpu));
  VkDevice display_gpu = my_instance.cod->display_gpu;

  TRACE("FamilyIndexCount: " <<  pCreateInfo->queueFamilyIndexCount);
  TRACE("Dev: " << GetKey(display_gpu));
  TRACE("Swapchainfunc: " << (void*) device_dispatch[GetKey(display_gpu)].CreateSwapchainKHR);

  VkSwapchainKHR backend;
  VkResult rc = device_dispatch[GetKey(display_gpu)].CreateSwapchainKHR(display_gpu, pCreateInfo, pAllocator, &backend);
  TRACE(">> Swapchain create done " << rc << ";" << (void*) backend);
  if(rc != VK_SUCCESS){
    return rc;
  }

  PrimusSwapchain *ch = new PrimusSwapchain(render_gpu, display_gpu, backend, pCreateInfo, info2.minImageCount, my_instance.cod);

  *pSwapchain = reinterpret_cast<VkSwapchainKHR>(ch);


  return rc;
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    if(swapchain == VK_NULL_HANDLE) { return;}
  PrimusSwapchain *ch = reinterpret_cast<PrimusSwapchain*>(swapchain);
  TRACE(">> Destroy swapchain: " << (void*) ch->backend);
  ch->stop();
  device_dispatch[GetKey(ch->display_device)].DestroySwapchainKHR(ch->display_device, ch->backend, pAllocator);
  delete ch;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages) {
  PrimusSwapchain *ch = reinterpret_cast<PrimusSwapchain*>(swapchain);

  *pSwapchainImageCount = ch->images.size();
  VkResult res = VK_SUCCESS;
  if(pSwapchainImages != nullptr) {
    res = VK_SUCCESS;
    for(size_t i = 0; i < *pSwapchainImageCount; i++){
      pSwapchainImages[i] = ch->images[i].render_image->img;
    }
    TRACE("Count: " << *pSwapchainImageCount);
  }
  return res;
}

const auto primus_start = std::chrono::steady_clock::now();

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {
  TRACE_PROFILING_EVENT(-1, "Acquire starting");
  PrimusSwapchain *ch = reinterpret_cast<PrimusSwapchain*>(swapchain);

  VkResult res;
  {
    Fence myfence{ch->display_device};

    res = device_dispatch[GetKey(ch->display_device)].AcquireNextImageKHR(ch->display_device, ch->backend, timeout, VK_NULL_HANDLE, myfence.fence, pImageIndex);
    TRACE_PROFILING_EVENT(*pImageIndex, "got image");

    myfence.await();
  }
  VkSubmitInfo qsi{};
  qsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  qsi.signalSemaphoreCount = 1;
  qsi.pSignalSemaphores = &semaphore;
  scoped_lock lock(*device_instance_info[GetKey(ch->render_queue)]->renderQueueMutex);
  device_dispatch[GetKey(ch->render_queue)].QueueSubmit(ch->render_queue, 1, &qsi, fence);
  TRACE_PROFILING_EVENT(*pImageIndex, "Acquire done");

  return res;
}

std::tuple<ssize_t, ssize_t, ssize_t> PrimusSwapchain::getImageMemories(){
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

  return std::make_tuple(render_local_mem, render_host_mem, display_host_mem);
}

void ImageWorker::createCommandBuffers(){
  {
    auto cpyImage = render_copy_image;
    auto srcImage = render_image->img;
    render_copy_command = std::make_shared<CommandBuffer>(swapchain. device);
    CommandBuffer &cmd = *render_copy_command;
    cmd.insertImageMemoryBarrier(
	cpyImage->img,
	VK_ACCESS_HOST_READ_BIT,		VK_ACCESS_TRANSFER_WRITE_BIT,
	VK_IMAGE_LAYOUT_UNDEFINED,		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	VK_PIPELINE_STAGE_HOST_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.insertImageMemoryBarrier(
	srcImage,
	VK_ACCESS_MEMORY_READ_BIT,		VK_ACCESS_TRANSFER_READ_BIT,
	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

    cmd.copyImage(srcImage, cpyImage->img, swapchain.imgSize);

    cmd.insertImageMemoryBarrier(
	cpyImage->img,
	VK_ACCESS_TRANSFER_WRITE_BIT,		VK_ACCESS_HOST_READ_BIT,
	VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,	VK_IMAGE_LAYOUT_GENERAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_HOST_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.insertImageMemoryBarrier(
	srcImage,
	VK_ACCESS_TRANSFER_READ_BIT,		VK_ACCESS_MEMORY_READ_BIT,
	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	VK_PIPELINE_STAGE_TRANSFER_BIT,		VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

    cmd.end();
  }

  {
    display_command = std::make_shared<CommandBuffer>(swapchain.display_device);
    CommandBuffer &cmd = *display_command;
    cmd.insertImageMemoryBarrier(
	display_src_image->img,
	VK_ACCESS_HOST_WRITE_BIT,	VK_ACCESS_TRANSFER_READ_BIT,
	VK_IMAGE_LAYOUT_GENERAL,	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	VK_PIPELINE_STAGE_HOST_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.insertImageMemoryBarrier(
	display_image,
	VK_ACCESS_MEMORY_READ_BIT,	VK_ACCESS_TRANSFER_WRITE_BIT,
	VK_IMAGE_LAYOUT_UNDEFINED,	VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.copyImage(display_src_image->img, display_image, swapchain.imgSize);

    cmd.insertImageMemoryBarrier(
	display_src_image->img,
	VK_ACCESS_TRANSFER_READ_BIT,	VK_ACCESS_HOST_WRITE_BIT,
	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,	VK_IMAGE_LAYOUT_GENERAL,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_HOST_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.insertImageMemoryBarrier(
	display_image,
	VK_ACCESS_TRANSFER_WRITE_BIT,	VK_ACCESS_MEMORY_READ_BIT,
	VK_IMAGE_LAYOUT_UNDEFINED,	VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	VK_PIPELINE_STAGE_TRANSFER_BIT,	VK_PIPELINE_STAGE_TRANSFER_BIT,
	VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
    cmd.end();
  }
}

void PrimusSwapchain::storeImage(uint32_t index, VkQueue queue, std::vector<VkSemaphore> wait_on, Fence &notify){
  images[index].render_copy_command->submit(queue, notify.fence, wait_on);
}

void ImageWorker::copyImageData(std::vector<VkSemaphore> sems){
  {
    auto rendered = render_copy_image->getMapped();
    auto display = display_src_image->getMapped();
    auto rendered_layout = render_copy_image->getLayout();
    auto display_layout = display_src_image->getLayout();
    auto rendered_start = rendered->data + rendered_layout.offset;
    auto display_start = display->data + display_layout.offset;
    if(rendered_layout.size/rendered_layout.rowPitch != display_layout.size/display_layout.rowPitch){
      TRACE("Layouts don't match at all");
      throw std::runtime_error("Layouts don't match at all");
    }
    TRACE_PROFILING_EVENT(index, "memcpy start");
    if(rendered_layout.rowPitch == display_layout.rowPitch){
      std::memcpy(display_start, rendered_start, rendered_layout.size);
    }else{
      VkDeviceSize display_offset = 0;
      VkDeviceSize minRowPitch = rendered_layout.rowPitch;
      if(display_layout.rowPitch < minRowPitch){
	minRowPitch = display_layout.rowPitch;
      }
      for(VkDeviceSize offset = 0; offset < rendered_layout.size; offset += rendered_layout.rowPitch){
	std::memcpy(display_start + display_offset, rendered_start + offset, minRowPitch);
	display_offset += display_layout.rowPitch;
      }
    }
    TRACE_PROFILING_EVENT(index, "memcpy done");
  }
  {
    std::unique_lock<std::mutex> lock(swapchain.queueMutex);
    if(display_command_fence){
      display_command_fence->await();
      display_command_fence->reset();
    }else{
      display_command_fence = std::unique_ptr<Fence>(new Fence(swapchain.display_device));
    }
    display_command->submit(swapchain.display_queue, display_command_fence->fence, {}, sems);
  }
}

void PrimusSwapchain::queue(VkQueue queue, const VkPresentInfoKHR* pPresentInfo){
  std::unique_lock<std::mutex> lock(queueMutex);

  auto workItem = QueueItem{queue, *pPresentInfo, pPresentInfo->pImageIndices[0]};
  storeImage(workItem.imgIndex, render_queue, std::vector<VkSemaphore>{pPresentInfo->pWaitSemaphores, pPresentInfo->pWaitSemaphores + pPresentInfo->waitSemaphoreCount}, images[workItem.imgIndex].render_copy_fence);

  work.push_back(std::move(workItem));
  has_work.notify_all();
}
void PrimusSwapchain::stop(){
  {
    std::unique_lock<std::mutex> lock(queueMutex);
    active = false;
    has_work.notify_all();
  }
  for(auto &thread: threads){
    thread->join();
    thread.reset();
  }
}
void PrimusSwapchain::present(const QueueItem &workItem){
    const auto index = workItem.imgIndex;
    images[index].render_copy_fence.await();
    images[index].render_copy_fence.reset();
    images[index].copyImageData({images[index].display_semaphore.sem});

    TRACE_PROFILING_EVENT(index, "copy queued");

    VkPresentInfoKHR p2 = {.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    p2.pSwapchains = &backend;
    p2.swapchainCount = 1;
    p2.pWaitSemaphores = &images[workItem.imgIndex].display_semaphore.sem;
    p2.waitSemaphoreCount = 1;
    p2.pImageIndices = &index;

    {
      std::unique_lock<std::mutex> lock(queueMutex);
      has_work.wait(lock, [this,&workItem](){return &workItem == &in_progress.front();});
      TRACE_PROFILING_EVENT(index, "submitting");
      VkResult res = device_dispatch[GetKey(display_queue)].QueuePresentKHR(display_queue, &p2);
      if(res != VK_SUCCESS) {
	TRACE("ERROR, Queue Present failed\n");
      }
      in_progress.pop_front();
      has_work.notify_all();
    }
}
void PrimusSwapchain::run(){
  while(true){
    QueueItem *workItem = nullptr;
    {
      std::unique_lock<std::mutex> lock(queueMutex);
      has_work.wait(lock, [this](){return !active || work.size() > 0;});
      if(!active) return;
      in_progress.push_back(std::move(work.front()));
      workItem = &in_progress.back();
      work.pop_front();
    }
    present(*workItem);
  }
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_QueueSubmit(VkQueue queue, uint32_t submitCount,
							 const VkSubmitInfo* pSubmits,
							 VkFence fence) {
  scoped_lock lock(*device_instance_info[GetKey(queue)]->renderQueueMutex);
  return device_dispatch[GetKey(queue)].QueueSubmit(queue, submitCount, pSubmits, fence);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
  scoped_lock lock(*device_instance_info[GetKey(queue)]->renderQueueMutex);
  const auto start = std::chrono::steady_clock::now();
  if(pPresentInfo->swapchainCount != 1){
    TRACE("Warning, presenting with multiple swapchains not implemented, ignoring");
  }

  PrimusSwapchain *ch = reinterpret_cast<PrimusSwapchain*>(pPresentInfo->pSwapchains[0]);
  double secs = std::chrono::duration_cast<std::chrono::duration<double>>(start - ch->lastPresent).count();
  TRACE_PROFILING_EVENT(pPresentInfo->pImageIndices[0], "QueuePresent");
  TRACE_PROFILING(" === Time between VkQueuePresents: " << secs << " -> " << 1/secs << " FPS");
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

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties* pQueueFamilyProperties) {
  VkPhysicalDevice phy = physicalDevice;
  instance_dispatch[GetKey(phy)].GetPhysicalDeviceQueueFamilyProperties(phy, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_QueueWaitIdle(VkQueue queue){
  scoped_lock lock(*device_instance_info[GetKey(queue)]->renderQueueMutex);
  device_dispatch[GetKey(queue)].QueueWaitIdle(queue);
}

VK_LAYER_EXPORT void VKAPI_CALL PrimusVK_DeviceWaitIdle(VkDevice device){
  auto &my_instance = *device_instance_info[GetKey(device)];
  device_dispatch[GetKey(device)].DeviceWaitIdle(device);
  device_dispatch[GetKey(my_instance.cod->display_gpu)].DeviceWaitIdle(my_instance.cod->display_gpu);
}

#include "primus_vk_forwarding_prototypes.h"

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
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumeratePhysicalDeviceGroups(
    VkInstance                                  instance,
    uint32_t*                                   pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties*            pPhysicalDeviceGroupProperties) {
  InstanceInfo &info = instance_info[GetKey(instance)];
  *pPhysicalDeviceGroupCount = 1;
  if(pPhysicalDeviceGroupProperties){
    pPhysicalDeviceGroupProperties[0].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
    pPhysicalDeviceGroupProperties[0].pNext = nullptr;
    pPhysicalDeviceGroupProperties[0].physicalDeviceCount = 1;
    pPhysicalDeviceGroupProperties[0].physicalDevices[0] = info.render;
    pPhysicalDeviceGroupProperties[0].subsetAllocation = VK_FALSE;
  }
  return VK_SUCCESS;
}
VK_LAYER_EXPORT VkResult VKAPI_CALL PrimusVK_EnumeratePhysicalDeviceGroupsKHR(
    VkInstance                                  instance,
    uint32_t*                                   pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties*            pPhysicalDeviceGroupProperties) {
  return PrimusVK_EnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
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
  GETPROCADDR(QueueSubmit);
  GETPROCADDR(DeviceWaitIdle);
  GETPROCADDR(QueueWaitIdle);
#define FORWARD(func) GETPROCADDR(func)
#include "primus_vk_forwarding.h"
#undef FORWARD
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
  GETPROCADDR(EnumeratePhysicalDeviceGroups);
  GETPROCADDR(EnumeratePhysicalDeviceGroupsKHR);
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
  GETPROCADDR(QueueSubmit);
  GETPROCADDR(DeviceWaitIdle);
  GETPROCADDR(QueueWaitIdle);
  GETPROCADDR(GetPhysicalDeviceQueueFamilyProperties);

#define FORWARD(func) GETPROCADDR(func)
#include "primus_vk_forwarding.h"
#undef FORWARD

  // GETPROCADDR(CreateXcbSurfaceKHR);
  {
    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, pName);
  }
}
