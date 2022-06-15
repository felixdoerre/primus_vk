#include <vulkan.h>
#include <dlfcn.h>

#include <GL/glx.h>

#include <string>
#include <memory>
#include <iostream>
#include <functional>

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion);


#ifndef NV_DRIVER_PATH
#define NV_DRIVER_PATH "/usr/lib/x86_64-linux-gnu/nvidia/libGL.so.1:libGLX_nvidia.so.0"
#endif
#ifndef NV_BUMBLEBEE_DISPLAY
#define NV_BUMBLEBEE_DISPLAY ":8"
#endif

typedef void* dlsym_fn(void *, const char*);

#define GLX_CONTEXT_MAJOR_VERSION_ARB		0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB		0x2092
typedef GLXContext (*GLXCREATECONTEXTATTRIBSARBPROC)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

typedef void *(*getproc )(const char* name);

class BBGLXContext {
public:
  Display *dpy;
  Window win;
  GLXContext ctx;
public:
  BBGLXContext() = default;
  BBGLXContext(const BBGLXContext &copy) = delete;
  BBGLXContext(const char* display){
    dpy = XOpenDisplay(display);
    if(!dpy){
      std::cerr << "Can't open bumblebee display.\n";
      return;
    }
    
    int nelements;
    GLXFBConfig *fbc = glXChooseFBConfig(dpy, DefaultScreen(dpy), 0, &nelements);
    static int attributeList[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1, GLX_BLUE_SIZE, 1, None };
    XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy),attributeList);

    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, 100, 100, 0, vi->depth, InputOutput, vi->visual, CWBorderPixel|CWColormap|CWEventMask, &swa);
    XFree(vi);

    GLXCREATECONTEXTATTRIBSARBPROC pfn_glXCreateContextAttribsARB = (GLXCREATECONTEXTATTRIBSARBPROC) glXGetProcAddress((const GLubyte*)"glXCreateContextAttribsARB");

    int attribs[] = {
      GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
      GLX_CONTEXT_MINOR_VERSION_ARB, 0,
      0};
    ctx = pfn_glXCreateContextAttribsARB(dpy, *fbc, 0, true, attribs);
    XFree(fbc);
  }
  ~BBGLXContext() {
    if(dpy != nullptr) {
      glXDestroyContext(dpy, ctx); 
      XDestroyWindow(dpy, win);
      XCloseDisplay(dpy);
    }
  }
  BBGLXContext& operator=(BBGLXContext&& other) noexcept {
    dpy = other.dpy;
    win = other.win;
    ctx = other.ctx;
    other.dpy = nullptr;
    return *this;
  }
  bool isValid(){
    return dpy != nullptr;
  }
  static void deleter(int *){
  }
  std::unique_ptr<void, std::function<void(void*)>> makeCurrent(){
    glXMakeCurrent (dpy, win, ctx);
    return std::unique_ptr<void, std::function<void(void*)>>(this, [this](void *p){
      glXMakeCurrent (dpy, 0, 0);
    });
  }
};
class VulkanIcd {
protected:
  VKAPI_ATTR PFN_vkVoidFunction (*instanceProcAddr) (VkInstance instance,
                                               const char* pName) = nullptr;
  VKAPI_ATTR PFN_vkVoidFunction (*phyProcAddr) (VkInstance instance,
                                               const char* pName) = nullptr;
  VKAPI_ATTR VkResult VKAPI_CALL (*negotiateVersion)(uint32_t* pSupportedVersion) = nullptr;
public:
  VulkanIcd() = default;
  virtual ~VulkanIcd() = default;
  virtual PFN_vkVoidFunction vk_icdGetInstanceProcAddr(
                                               VkInstance instance,
                                               const char* pName){
    return instanceProcAddr(instance, pName);
  }
  virtual PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
							     const char* pName) {
    return phyProcAddr(instance, pName);
  }
  virtual VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    return negotiateVersion(pSupportedVersion);
  }

};
class ExternalVulkanIcd: public VulkanIcd {
  void *libnv;
public:
  ExternalVulkanIcd(){
    libnv = dlopen("libGLX_nvidia.so.0", RTLD_LOCAL | RTLD_LAZY);
    auto fn2 = [this](const char* name) {
       return (void *) dlsym(this->libnv, name);
    };
    instanceProcAddr = (decltype(instanceProcAddr)) fn2("vk_icdGetInstanceProcAddr");
    phyProcAddr = (decltype(phyProcAddr)) fn2("vk_icdGetPhysicalDeviceProcAddr");
    negotiateVersion = (decltype(negotiateVersion)) fn2("vk_icdNegotiateLoaderICDInterfaceVersion");
  }
  ~ExternalVulkanIcd(){
    dlclose(libnv);
  }
  VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    char *prev = getenv("DISPLAY");
    std::string old{prev};
    setenv("DISPLAY", NV_BUMBLEBEE_DISPLAY, 1);
    auto res = negotiateVersion(pSupportedVersion);
    setenv("DISPLAY",old.c_str(), 1);
    return res;
  }
};
PFN_vkVoidFunction getOverrideFn(const char *pName);
class InternalVulkanIcd: public VulkanIcd {
public:
  PFN_vkGetDeviceProcAddr getDeviceProcAddr;

  PFN_vkCreateInstance createInstance;
  PFN_vkDestroyInstance destroyInstance;
  PFN_vkCreateDevice createDevice;
  PFN_vkDestroyDevice destroyDevice;
  PFN_vkGetDeviceQueue getDeviceQueue;
  PFN_vkCreateSwapchainKHR createSwapchainKHR;
  PFN_vkDestroySwapchainKHR destroySwapchainKHR;
  PFN_vkQueuePresentKHR queuePresentKHR;
  PFN_vkQueueSubmit queueSubmit;
  //PFN_vkAllocateMemory;
  //PFN_vkBindBufferMemory
  //PFN_vkBindImageMemory;
  //PFN_vkAcquireNextImageKHR;
public:
  InternalVulkanIcd(BBGLXContext &glx){
    auto guard = glx.makeCurrent();
    getproc fn2 = (getproc) glXGetProcAddress((const GLubyte*) "glGetVkProcAddrNV");
    // getproc fn2 = (getproc) glXGetProcAddress((const GLubyte*) "ex7991765ed");
    void *glfn = fn2("vk_icdGetInstanceProcAddr");

    instanceProcAddr = (decltype(instanceProcAddr)) fn2("vk_icdGetInstanceProcAddr");
    phyProcAddr = (decltype(phyProcAddr)) fn2("vk_icdGetPhysicalDeviceProcAddr");
    negotiateVersion = (decltype(negotiateVersion)) fn2("vk_icdNegotiateLoaderICDInterfaceVersion");

    createInstance = (decltype(createInstance)) fn2("vkCreateInstance");
    destroyInstance = (decltype(destroyInstance)) fn2("vkDestroyInstance");
    createDevice = (decltype(createDevice)) fn2("vkCreateDevice");
    destroyDevice = (decltype(destroyDevice)) fn2("vkDestroyDevice");
    getDeviceProcAddr = (decltype(getDeviceProcAddr)) fn2("vkGetDeviceProcAddr");
    getDeviceQueue = (decltype(getDeviceQueue)) fn2("vkGetDeviceQueue");
    createSwapchainKHR = (decltype(createSwapchainKHR)) fn2("vkCreateSwapchainKHR");
    destroySwapchainKHR = (decltype(destroySwapchainKHR)) fn2("vkDestroySwapchainKHR");
    queuePresentKHR = (decltype(queuePresentKHR)) fn2("vkQueuePresentKHR");
    queueSubmit = (decltype(queueSubmit)) fn2("vkQueueSubmit");
  }
  virtual PFN_vkVoidFunction vk_icdGetInstanceProcAddr(
                                               VkInstance instance,
                                               const char* pName){
    auto internalFn = instanceProcAddr(instance, pName);
    if(internalFn != nullptr){
      auto overrideFnc = getOverrideFn(pName);
      if(overrideFnc != nullptr){
	return overrideFnc;
      }
    }
    return internalFn;
  }
  VkResult vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    return VulkanIcd::vk_icdNegotiateLoaderICDInterfaceVersion(pSupportedVersion);
  }
  
};

class StaticInitialize {
  void *nvDriver;
  void *glLibGL;
public:
  BBGLXContext glx;
  std::unique_ptr<VulkanIcd> icd;

public:
  StaticInitialize(){
    // Load libGL from LD_LIBRARY_PATH before loading the NV-driver (unluckily also named libGL
    // This ensures that ld.so will find this libGL before the Nvidia one, when
    // again asked to load libGL.
    glLibGL = dlopen("libGL.so.1", RTLD_GLOBAL | RTLD_NOW);
    glx = BBGLXContext(NV_BUMBLEBEE_DISPLAY);
    if(!glx.isValid()){
	return;
    }
    // icd = std::unique_ptr<ExternalVulkanIcd>(new ExternalVulkanIcd());
    icd = std::unique_ptr<InternalVulkanIcd>(new InternalVulkanIcd(glx));
    
  }
  ~StaticInitialize(){
    dlclose(glLibGL);
  }
  bool IsInited(){
    return icd != nullptr;
  }
  InternalVulkanIcd &internal(){
    return *reinterpret_cast<InternalVulkanIcd*>(icd.get());
  }
};

StaticInitialize init;

template<typename PFN, PFN InternalVulkanIcd::*p, typename... Args>
VKAPI_ATTR auto VKAPI_CALL forward(Args... args) -> decltype((init.internal().*p)(args...)) {
  auto guard = init.glx.makeCurrent();
  return (init.internal().*p)(args...);
}

template<auto p, typename PFN = typename std::remove_reference<decltype(init.internal().*p)>::type>
auto forwarder() -> PFN {
  return &forward<PFN, p>;
}
PFN_vkVoidFunction vk_GetDeviceProcAddr(
                                               VkDevice device,
                                               const char* pName){
  auto ret = init.internal().getDeviceProcAddr(device, pName);
  if(ret != nullptr){
    auto r2 = getOverrideFn(pName);
    if(r2 != nullptr){
      return r2;
    }
  }
  return ret;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
                                               VkInstance instance,
                                               const char* pName){
  if (!init.IsInited()) return nullptr;
  return init.icd->vk_icdGetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction getOverrideFn(const char *pName){
  std::string name = pName;
  if(name == "vkGetInstanceProcAddr") {
    return (PFN_vkVoidFunction) vk_icdGetInstanceProcAddr;
  }else if(name == "vkGetDeviceProcAddr") {
    return (PFN_vkVoidFunction) vk_GetDeviceProcAddr;
  }else if(name == "vkCreateInstance") {
    return (PFN_vkVoidFunction) forwarder<&InternalVulkanIcd::createInstance>();
  }else if(name == "vkDestroyInstance") {
    return (PFN_vkVoidFunction) forwarder<&InternalVulkanIcd::destroyInstance>();
  }else if(name == "vkCreateDevice") {
    return (PFN_vkVoidFunction) forwarder<&InternalVulkanIcd::createDevice>();
  }else if(name == "vkDestroyDevice") {
    return (PFN_vkVoidFunction) forwarder<&InternalVulkanIcd::destroyDevice>();
  }else if(name == "vkGetDeviceQueue") {
    return (PFN_vkVoidFunction) forwarder<&InternalVulkanIcd::getDeviceQueue>();
  }else if(name == "vkCreateSwapchainKHR") {
    return (PFN_vkVoidFunction) forwarder<&InternalVulkanIcd::createSwapchainKHR>();
  }else if(name == "vkDestroySwapchainKHR") {
    return (PFN_vkVoidFunction) forwarder<&InternalVulkanIcd::destroySwapchainKHR>();
  }else if(name == "vkQueuePresentKHR") {
    return (PFN_vkVoidFunction) forwarder<&InternalVulkanIcd::queuePresentKHR>();
  }else if(name == "vkQueueSubmit") {
    return (PFN_vkVoidFunction) forwarder<&InternalVulkanIcd::queueSubmit>();
  }
  return nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance instance,
						    const char* pName){
  if (!init.IsInited()) return nullptr;
  return init.icd->vk_icdGetPhysicalDeviceProcAddr(instance, pName);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion){
  if (!init.IsInited()) {
    return VK_ERROR_INCOMPATIBLE_DRIVER;
  }
  return init.icd->vk_icdNegotiateLoaderICDInterfaceVersion(pSupportedVersion);
}
