# Primus-vk

This Vulkan layer can be used to do GPU offloading. Typically you want to display an image rendered on a more powerful GPU on a display managed by an internal GPU.

It is basically the same as Primus for OpenGL (https://github.com/amonakov/primus). However it does not wrap the Vulkan API from the application but is directly integrated into Vulkan as a layer (which seems to be the intendend way to implement such logic).

## Idea

Just as the OpenGL-Primus: Let the application talk to the primary display and transparently map API calls so that the application thinks, it renders using the primary display, however the `VkDevice` (and `VkImage`s) comes from the rendering GPU.
When the application wants to swap frames, copy the image over to the integrated GPU and display it there.

## Why do we need to copy the Image so often?
As far as I can tell `VkImage` (and `VkMemory`) objects may not be shared beween different physical devices. So there is not really another way than using `memcpy` on the images when memmapped into main memory.

Additinonally, only images with `VK_IMAGE_TILING_OPTIMAL` can be rendered to and presentend and only images with `VK_IMAGE_TILING_LINEAR` can be mapped to main memory to be copied. So I see no better way than copying the image 3 times from render target to display. On my machine the `memcpy` from an external device was pretty clearly the bottleneck. So it is not really the copying of the image, but the transfer from rendering GPU into main memory.

An idea might be to use `VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT` to map one device's memory and use that directly on the other device (or import host-allocated memory on both devices). However that is not implemented yet.

## Development Status

This layer works for the applications I tested it with, but has still some technical difficulties (see Technical Limitations). Additionally the image copy still introduces too much overhead.
However this layer should already be usable with most applications.

## Technical Limitations

1. The layer might deadlock on swapchain creation. I currently have no easy way to fix this. However real applications (and not demos) tend to spend enough time between `vkCreateDevice` and `vkCreateSwapchainKHR` that this deadlock never occurs. This is due to the vk_layer limitation that creating dispatchable objects is quite complex. I have the problem, that I need a new `VkDevice` from a `VkPhysicalDevice` where the application never calls `vkCreateDevice` upon. So the code from the Vulkan Loader that builds the layer/ICD chain is only executed once. The only way to call it again is to call the Loader again which might deadlock in the Loader global lock.

2. The NVIDIA driver always connect to the "default" X-Display to verify that it has the NV-GLX extensions availible. Otherwise the NVIDIA-vulkan-icd driver disables itself. For testing an intermediate solution is to modify the demo application to always use ":0" and set DISPLAY to ":8" to make the NV-Driver happy. However this approach does work on general applications that cannot be modified. So this issue has to be solved in the graphics driver.

3. Currently under Debian unstable the nvidia-icd is registered with a non-absolute library path in `/usr/share/vulkan/icd.d/nvidia_icd.json`. Replace `libGL.so.1` with `/usr/lib/x86_64-linux-gnu/nvidia/libGL.so.1` there to always load the intended Vulkan driver.


4. When running an applications with DXVK and wine, wine loads both Vulkan and OpenGL. This creates a problem as:
	1. Wine loads Vulkan, which loades the integrated GPU's ICD, the Nvidia ICD (contained in Nvidia's libGL.so on my system), Primus-VK and potentially more.
	2. Wine loads OpenGL, which should be satisfied by OpenGL-Primus. However for whatever reason wine directly gets Nvidia's libGL which fails to provide an OpenGL context for the primary X screen.
	This needs to be prevented by forcing wine to load Primus' libGL.

Issues 2.,3. and 4. can be worked around by compiling `libnv_vulkan_wrapper.so` and registering it instead of nvidia's `libGL.so.1` in `/usr/share/vulkan/icd.d/nvidia_icd.json`.

## Installation
### Locally
Create the folder `~/.local/share/vulkan/implicit_layer.d` and copy `primus_vk.json` there with the path adjusted to the location of the shared object.

### System-wide
Copy `primus_vk.json` to `/usr/share/vulkan/implicit_layer.d` and adjust the path.

## Howto

1. Use `make libprimus_vk.so libnv_vulkan_wrapper.so` to compile Primus-vk and `libnv_vulkan_wrapper.so` (check that the path to the nvidia-driver in `nv_vulkan_wrapper.so` is correct).
2. Patch path in `/usr/share/vulkan/icd.d/nvidia_icd.json` to point to the compiled `libnv_vulkan_wrapper.so`.
3. Install `primus_vk.json` and adjust path.
4. Run `ENABLE_PRIMUS_LAYER=1 optirun vulkan-smoketest`.
5. If you want to have more than 30 FPS and don't care if `primus_vk` uses more CPU resources, set `PRIMUS_VK_MULTITHREADING=1`

I tested this on Debian unstable.

### Debian Packages that I used:

```
bumblebee-nvidia                                            3.2.1-17
nvidia-driver                                               390.77-1
nvidia-nonglvnd-vulkan-icd:amd64                            390.77-1
nvidia-nonglvnd-vulkan-icd:i386                             390.77-1
primus                                                      0~20150328-6
mesa-vulkan-drivers:amd64                                   18.1.7-1
```

For testing a Windows DX11-Application, I used:
```
wine32-development:i386                                     3.14-1
wine64-development                                          3.14-1
```
and dxvk-0.7 inside the wineprefix.

### Arch Linux

Notes for running on Arch Linux:

* nv_vulkan_wrapper.cpp: Change nvDiver path to `/usr/lib/libGLX_nvidia.so.0`
* primus_vk.cpp: add: `#include "vk_layer_utils.h"` (on Debian the contents are included in some other header and there is no "vk_layer_utils.h")

### RPM package

Leonid Maksymchuk built RPM packaging scripts for primus-vk which can be found in his [repository](https://github.com/leonmaxx/primus-vk-rpm).

## Credits

This layer is based on the sample layer available under https://github.com/baldurk/sample_layer. The guide that goes along with it is [https://renderdoc.org/vulkan-layer-guide.html](https://renderdoc.org/vulkan-layer-guide.html).
