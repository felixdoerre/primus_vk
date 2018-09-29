#include <vulkan.h>
#include <dlfcn.h>

#include <iostream>

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion);

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

    nvDriver = dlopen("/usr/lib/x86_64-linux-gnu/nvidia/current/libGL.so.1", RTLD_LOCAL | RTLD_LAZY);
    typedef void* (*dlsym_fn)(void *, const char*);
    static dlsym_fn real_dlsym = (dlsym_fn) dlsym(dlopen("libdl.so.2", RTLD_LAZY), "dlsym");
    instanceProcAddr = (decltype(instanceProcAddr)) real_dlsym(nvDriver, "vk_icdGetInstanceProcAddr");
    phyProcAddr = (decltype(phyProcAddr)) real_dlsym(nvDriver, "vk_icdGetPhysicalDeviceProcAddr");
    negotiateVersion = (decltype(negotiateVersion)) real_dlsym(nvDriver, "vk_icdNegotiateLoaderICDInterfaceVersion");
  }
  ~StaticInitialize(){
    dlclose(nvDriver);
    dlclose(glLibGL);
  }
};

StaticInitialize init;

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
                                               VkInstance instance,
                                               const char* pName){
  auto res = init.instanceProcAddr(instance, pName);
  return res;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
						    const char* pName){
  
  auto res = init.phyProcAddr(instance, pName);
  return res;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion){
  char *prev = getenv("DISPLAY");
  std::string old{prev};
  setenv("DISPLAY", ":8", 1);
  auto res = init.negotiateVersion(pSupportedVersion);
  setenv("DISPLAY",old.c_str(), 1);
  return res;
}
