#include <vulkan.h>
#include <dlfcn.h>

#include <GL/gl.h>
#include <GL/glx.h>

#include <string>
#include <memory>
#include <iostream>

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
      std::cout << "Can't open bumblebee display.\n";
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
  void makeCurrent(){
    glXMakeCurrent (dpy, win, ctx);
  }
};
#include <thread>
class StaticInitialize {
  void *nvDriver;
  void *glLibGL;
  BBGLXContext glx;
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
    glx = BBGLXContext(NV_BUMBLEBEE_DISPLAY);
    if(!glx.isValid()){
	return;
    }

    glx.makeCurrent();
    getproc fn2 = (getproc) glXGetProcAddress((const GLubyte*) "glGetVkProcAddrNV");
    void *glfn = fn2("vk_icdGetInstanceProcAddr");

    instanceProcAddr = (decltype(instanceProcAddr)) fn2("vk_icdGetInstanceProcAddr");
    phyProcAddr = (decltype(phyProcAddr)) fn2("vk_icdGetPhysicalDeviceProcAddr");
    negotiateVersion = (decltype(negotiateVersion)) fn2("vk_icdNegotiateLoaderICDInterfaceVersion");
  }
  ~StaticInitialize(){
    dlclose(glLibGL);
  }
  bool IsInited(){
    return negotiateVersion != nullptr;
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
  
  return init.negotiateVersion(pSupportedVersion);
}
