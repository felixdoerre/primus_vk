#include <GL/glx.h>
#include <GL/gl.h>
#include <unistd.h>
#include <iostream>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

#include <dlfcn.h>

#define GLX_CONTEXT_MAJOR_VERSION_ARB       0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB       0x2092
typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);

class VulkanContext {
  VkInstance instance;
public:
  VulkanContext();
  VulkanContext(const VulkanContext&) = delete;
  ~VulkanContext();
};
#define VK_CHECK()     if(reply != VK_SUCCESS){ \
      throw std::runtime_error("Vulkan operation failed with code: " + std::to_string(reply)); \
    }


#define GL_FUNCTIONS GL_FUNCTION(glClearColor);\
  GL_FUNCTION(glClear);\
  GL_FUNCTION(glXChooseFBConfig);\
  GL_FUNCTION(glXCreateContext);\
  GL_FUNCTION(glXDestroyContext);\
  GL_FUNCTION(glXGetProcAddress);\
  GL_FUNCTION(glXGetVisualFromFBConfig);\
  GL_FUNCTION(glXMakeCurrent);\
  GL_FUNCTION(glXQueryExtensionsString);\
  GL_FUNCTION(glXSwapBuffers);\


struct GLLib {
#define GL_FUNCTION(x) decltype(&x) ptr_##x;
  GL_FUNCTIONS
#undef GL_FUNCTION
  GLLib() {
    void* handle = dlopen("libGL.so.1", RTLD_NOW|RTLD_GLOBAL);
#define GL_FUNCTION(x) ptr_##x = (decltype(&x)) dlsym(handle, #x)
    GL_FUNCTIONS
#undef GL_FUNCTION
  }
};

const auto self = std::string{"PrimusVK-diagnostic: "};

VulkanContext::VulkanContext(){
  std::cout << self << "Creating Vulkan instance" << std::endl;
  VkInstanceCreateInfo instanceCreateInfo = {};
  instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceCreateInfo.pNext = NULL;
  instanceCreateInfo.pApplicationInfo = nullptr;
  auto reply = vkCreateInstance(&instanceCreateInfo, nullptr, &instance);
  VK_CHECK();

  uint32_t gpuCount;
  reply = vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
  VK_CHECK();
  // Enumerate devices
  std::vector<VkPhysicalDevice> physicalDevices(gpuCount);
  vkEnumeratePhysicalDevices(instance, &gpuCount, physicalDevices.data());
  VK_CHECK();
  for ( auto &device : physicalDevices) {
    VkPhysicalDeviceProperties deviceProperties;
    vkGetPhysicalDeviceProperties(device, &deviceProperties);
    std::cout << self << "Device: " << deviceProperties.deviceName << std::endl;
    std::cout << self << " Type: " << deviceProperties.deviceType << std::endl;
    std::cout << self << " API: " << (deviceProperties.apiVersion >> 22) << "." << ((deviceProperties.apiVersion >> 12) & 0x3ff) << "." << (deviceProperties.apiVersion & 0xfff) << std::endl;
    uint32_t queues;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queues, nullptr);
    std::vector<VkQueueFamilyProperties> data(queues);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queues, data.data());
    std::cout << self << "   Queues: " << queues << std::endl;

    VkDeviceQueueCreateInfo queue1{};
    queue1.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue1.queueFamilyIndex = 0;
    queue1.queueCount = 1;
    float prio = 1;
    queue1.pQueuePriorities = &prio;
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pQueueCreateInfos = &queue1;
    createInfo.queueCreateInfoCount = 1;
    VkDevice dev;
    VkResult res = vkCreateDevice(device, &createInfo, nullptr, &dev);
    if(res == VK_SUCCESS) {
      std::cout << "Device creation succeeded\n";
    } else {
      std::cout << "Device creation failed: " << res << "\n";
    }
    vkDestroyDevice(dev, nullptr);
  }
}
VulkanContext::~VulkanContext(){
  std::cout << self << "Destroying Vulkan: " << instance << std::endl;
  vkDestroyInstance(instance, nullptr);
}

class XWindowContext;
class GLContext {
  GLXContext ctx;
  XWindowContext &data;
  std::shared_ptr<GLLib> gl;
public:
  GLContext(XWindowContext &data);
  void drawSample();
  ~GLContext();
};

class XWindowContext {
public:
  Display *display;
  XVisualInfo *vi;

  Window win;
  GLXFBConfig fbconfig;
  std::shared_ptr<GLLib> gl = std::make_shared<GLLib>();

  XWindowContext(Display *display): display(display){
    const char *extensions = gl->ptr_glXQueryExtensionsString(display, DefaultScreen(display));
    std::cout << self << extensions << std::endl;

    static int visual_attribs[] =
    {
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_DOUBLEBUFFER, true,
        GLX_RED_SIZE, 1,
        GLX_GREEN_SIZE, 1,
        GLX_BLUE_SIZE, 1,
        None
     };

    std::cout << self << "Getting framebuffer config" << std::endl;
    int fbcount;
    GLXFBConfig *fbc = gl->ptr_glXChooseFBConfig(display, DefaultScreen(display), visual_attribs, &fbcount);
    if (!fbc) {
      throw std::runtime_error("Failed to retrieve a framebuffer config");
    }
    fbconfig = fbc[0];

    vi = gl->ptr_glXGetVisualFromFBConfig(display, fbconfig);

    XSetWindowAttributes swa;
    swa.colormap = XCreateColormap(display, RootWindow(display, vi->screen), vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask;

    std::cout << self << "Creating window" << std::endl;
    win = XCreateWindow(display, RootWindow(display, vi->screen), 0, 0, 100, 100, 0, vi->depth, InputOutput, vi->visual, CWBorderPixel|CWColormap|CWEventMask, &swa);
    if (!win) {
      throw std::runtime_error("Failed to create window.");
    }
    XMapWindow(display, win);
  }
};

GLContext::GLContext(XWindowContext &data): data(data), gl(data.gl){
  // Create an oldstyle context first, to get the correct function pointer for glXCreateContextAttribsARB
  GLXContext ctx_old = gl->ptr_glXCreateContext(data.display, data.vi, 0, GL_TRUE);
  const auto glXCreateContextAttribsARB =  (glXCreateContextAttribsARBProc)gl->ptr_glXGetProcAddress((const GLubyte*)"glXCreateContextAttribsARB");
  gl->ptr_glXMakeCurrent(data.display, 0, 0);
  gl->ptr_glXDestroyContext(data.display, ctx_old);

  if (glXCreateContextAttribsARB == NULL) {
    throw std::runtime_error("glXCreateContextAttribsARB entry point not found. Aborting.");
  }

  static int context_attribs[] =
    {
      GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
      GLX_CONTEXT_MINOR_VERSION_ARB, 0,
      None
    };

  std::cout << self << "Creating context" << std::endl;
  ctx = glXCreateContextAttribsARB(data.display, data.fbconfig, NULL, true, context_attribs);
  if (!ctx) {
      throw std::runtime_error("Failed to create GL3 context.");
  }

  gl->ptr_glXMakeCurrent(data.display, data.win, ctx);
}
void GLContext::drawSample(){
  std::cout << self << "Rendering with GL" << std::endl;
  gl->ptr_glClearColor (0, 0.5, 1, 1);
  gl->ptr_glClear (GL_COLOR_BUFFER_BIT);
  gl->ptr_glXSwapBuffers (data.display, data.win);

  auto toSleep = timespec{};
  toSleep.tv_nsec=200000000;
  nanosleep(&toSleep, nullptr);

  gl->ptr_glClearColor (1, 0.5, 0, 1);
  gl->ptr_glClear (GL_COLOR_BUFFER_BIT);
  gl->ptr_glXSwapBuffers (data.display, data.win);

  nanosleep(&toSleep, nullptr);
}
GLContext::~GLContext(){
  std::cout << self << "Destroying GL context" << std::endl;
  gl->ptr_glXMakeCurrent(data.display, 0, 0);
  gl->ptr_glXDestroyContext(data.display, ctx);
}

int main (int argc, char ** argv) {
  Display *display = XOpenDisplay(0);
  for(int i = 1; i < argc; i++){
    std::string arg = argv[i];
    if(arg == "gl"){
      std::cout << self << "Loading GL." << std::endl;
      auto winContext = std::make_shared<XWindowContext>(display);
      GLContext context = GLContext{*winContext};
      context.drawSample();
    } else if(arg == "vulkan") {
      VulkanContext context;
    }
  }
  return 0;
}
