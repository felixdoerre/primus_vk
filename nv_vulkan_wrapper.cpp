#include <vulkan.h>
#include <dlfcn.h>

#include <string>
#include <iostream>

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion);


#ifndef NV_DRIVER_PATH
#define NV_DRIVER_PATH "/usr/lib/x86_64-linux-gnu/nvidia/libGL.so.1:libGLX_nvidia.so.0"
#endif
#ifndef NV_BUMBLEBEE_DISPLAY
#define NV_BUMBLEBEE_DISPLAY "hello"
#endif

typedef void* dlsym_fn(void *, const char*);

class StaticInitialize {
  void *nvDriver;
  void *glLibGL;
public:
  VKAPI_ATTR PFN_vkVoidFunction (*instanceProcAddr) (VkInstance instance,
                                               const char* pName);
  VKAPI_ATTR PFN_vkVoidFunction (*phyProcAddr) (VkInstance instance,
                                               const char* pName);
  VKAPI_ATTR VkResult VKAPI_CALL (*negotiateVersion)(uint32_t* pSupportedVersion);
public:
  StaticInitialize(){
    // Load libGL from LD_LIBRARY_PATH before loading the NV-driver (unluckily also named libGL
    // This ensures that ld.so will find this libGL before the Nvidia one, when
    // again asked to load libGL.
    glLibGL = dlopen("libGL.so.1", RTLD_GLOBAL | RTLD_NOW);

    std::string drivers(NV_DRIVER_PATH);
    while(!nvDriver && drivers.size() > 0){
      auto end = drivers.find(':');
      if(end == std::string::npos) {
	nvDriver = dlopen(drivers.c_str(), RTLD_LOCAL | RTLD_LAZY);
	drivers = "";
      } else {
	std::string this_driver = drivers.substr(0, end);
	nvDriver = dlopen(this_driver.c_str(), RTLD_LOCAL | RTLD_LAZY);
	drivers = drivers.substr(end+1);
      }
    }
    if(!nvDriver) {
      std::cerr << "PrimusVK: ERROR! Nvidia driver could not be loaded from '" NV_DRIVER_PATH "'.\n";
      return;
    }
    void *libdl = dlopen("libdl.so.2", RTLD_LAZY);
    // We explicitly want the real dlsym from libdl.so.2 because there are LD_PRELOAD libraries
    // that override dlsym and mess with the return values. We explicitly ask for the real
    // dlsym function, just to be safe.
    dlsym_fn *real_dlsym = (dlsym_fn*) dlsym(libdl, "dlsym");
    instanceProcAddr = (decltype(instanceProcAddr)) real_dlsym(nvDriver, "vk_icdGetInstanceProcAddr");
    phyProcAddr = (decltype(phyProcAddr)) real_dlsym(nvDriver, "vk_icdGetPhysicalDeviceProcAddr");
    negotiateVersion = (decltype(negotiateVersion)) real_dlsym(nvDriver, "vk_icdNegotiateLoaderICDInterfaceVersion");
    dlclose(libdl);
  }
  ~StaticInitialize(){
    if(nvDriver)
      dlclose(nvDriver);
    dlclose(glLibGL);
  }
  bool IsInited(){
    return nvDriver != nullptr;
  }
};

StaticInitialize init;

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
                                               VkInstance instance,
                                               const char* pName){
  if (!init.IsInited()) return nullptr;
  auto res = init.instanceProcAddr(instance, pName);
  return res;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
						    const char* pName){
  if (!init.IsInited()) return nullptr;
  auto res = init.phyProcAddr(instance, pName);
  return res;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion){
  if (!init.IsInited()) {
    return VK_ERROR_INCOMPATIBLE_DRIVER;
  }
  char *prev = getenv("DISPLAY");
  std::string old{prev};
  setenv("DISPLAY", NV_BUMBLEBEE_DISPLAY, 1);
  auto res = init.negotiateVersion(pSupportedVersion);
  setenv("DISPLAY",old.c_str(), 1);
  return res;
}
