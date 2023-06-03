# Primus-vk

This Vulkan layer can be used to do GPU offloading. Typically you want to display an image rendered on a more powerful GPU on a display managed by an internal GPU.

It is basically the same as Primus for OpenGL (https://github.com/amonakov/primus). However it does not wrap the Vulkan API from the application but is directly integrated into Vulkan as a layer (which seems to be the intendend way to implement such logic).

## Usage

First you need to install `primus_vk`:
 * On Archlinux there are official packages ([for 64-bit games](https://archlinux.org/packages/extra/x86_64/primus_vk/), [for 32-bit games](https://www.archlinux.org/packages/multilib/x86_64/lib32-primus_vk/)).
 * On Debian (from bullseye on) you should use `primus-vk-nvidia` (which recommends also the 32-bit variants of those packages for 32-bit games), which already is preconfigured for the Nvidia dedicated + Intel integrated graphics setup. When you have a different setup, you should install just `primus-vk` (which installs only the bare `primus_vk`-library and no graphics drivers), and install the Vulkan drivers, you need manually.
 * For Fedora there are [unofficial packages](https://copr.fedorainfracloud.org/coprs/yura/primus-vk/).
 * For other distributions you will likely need to [manually install](#installation) `primus_vk`.

To run an application with `primus_vk` prefix the command with `pvkrun` (which in the easiest case is just `ENABLE_PRIMUS_LAYER=1 optirun`). So instead of running `path/to/application`, invoke `pvkrun path/to/application` instead. You should be able to use `pvkrun` for all applications, independently of them using Vulkan, OpenGL or both.

By default `primus_vk` chooses a graphics card marked as `dedicated` and one not marked as `dedicated`. If that does not fit on your scenario, you need to specify the devices used for rendering and displaying manually. You can use `PRIMUS_VK_DISPLAYID` and `PRIMUS_VK_RENDERID` and give them the `deviceID`s from `optirun env DISPLAY=:8 vulkaninfo`. That way you can force `primus_vk` to work in a variety of different scenarios (e.g. having two dedicated graphics cards and rendering on one, while displaying on the other).


## Idea

Just as the OpenGL-Primus: Let the application talk to the primary display and transparently map API calls so that the application thinks, it renders using the primary display, however the `VkDevice` (and `VkImage`s) comes from the rendering GPU.
When the application wants to swap frames, copy the image over to the integrated GPU and display it there.

## Why do we need to copy the Image so often?
As far as I can tell `VkImage` (and `VkMemory`) objects may not be shared beween different physical devices. So there is not really another way than using `memcpy` on the images when memmapped into main memory.

Additionally, only images with `VK_IMAGE_TILING_OPTIMAL` can be rendered to and presentend and only images with `VK_IMAGE_TILING_LINEAR` can be mapped to main memory to be copied. So I see no better way than copying the image 3 times from render target to display. On my machine the `memcpy` from an external device was pretty clearly the bottleneck. So it is not really the copying of the image, but the transfer from rendering GPU into main memory.

An idea might be to use `VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT` to map one device's memory and use that directly on the other device (or import host-allocated memory on both devices). However that is not implemented yet.

## Dependencies
This layer requires two working vulkan drivers. The only hardware that I have experience with are Intel Integrated Graphics + Nvidia. However it should theoretically work with any other graphics setup of two vulkan-compatible graphics devices. For the Nvidia graphics card, both the "nonglvd" and the "glvnd" proprietary driver seem to work, however the "nonglvnd"-driver seems to be broken around `430.64` and is removed in newer versions.

To use this layer you will require something similar to bumblebee to poweron/off the dedicated graphics card.

Due to a bug/missing feature in the Vulkan Loader you will need `Vulkan/libvulkan >= 1.1.108`. If you have an older system you can try primus_vk version 1.1 which contains an ugly workaround for that issue and is therefore compatible with older Vulkan versions.


## Development Status

This layer works for all the applications I tested it with, but uses a fair share of CPU resorces for copying.

## Technical Limitations

1. The NVIDIA driver always connect to the "default" X-Display to verify that it has the NV-GLX extensions availible. Otherwise the NVIDIA-vulkan-icd driver disables itself. For testing an intermediate solution is to modify the demo application to always use ":0" and set DISPLAY to ":8" to make the NV-Driver happy. However this approach does work on general applications that cannot be modified. So this issue has to be solved in the graphics driver.

2. Currently under Debian unstable the nvidia-icd is registered with a non-absolute library path in `/usr/share/vulkan/icd.d/nvidia_icd.json`. Replace `libGL.so.1` with `/usr/lib/x86_64-linux-gnu/nvidia/libGL.so.1` there to always load the intended Vulkan driver.

3. When running an applications with DXVK and wine, wine loads both Vulkan and OpenGL. This creates a problem as:
	1. Wine loads Vulkan, which loades the integrated GPU's ICD, the Nvidia ICD (contained in Nvidia's libGL.so on my system), Primus-VK and potentially more.
	2. Wine loads OpenGL, which should be satisfied by OpenGL-Primus. However for whatever reason wine directly gets Nvidia's libGL which fails to provide an OpenGL context for the primary X screen.
	This needs to be prevented by forcing wine to load Primus' libGL.

Issues 1.,2. and 3. can be worked around by compiling `libnv_vulkan_wrapper.so` and registering it instead of nvidia's `libGL.so.1` in `/usr/share/vulkan/icd.d/nvidia_icd.json`.

## Installation
### Locally
Create the folder `~/.local/share/vulkan/implicit_layer.d` and copy `primus_vk.json` there with the path adjusted to the location of the shared object.

### System-wide
Copy `primus_vk.json` to `/usr/share/vulkan/implicit_layer.d` and adjust the path.

## Howto
1. Install the correct vulkan icds (i.e. intel/mesa, nvidia, amd, depending on your hardware).
2. Use `make libprimus_vk.so libnv_vulkan_wrapper.so` to compile Primus-vk and `libnv_vulkan_wrapper.so` (check that the path to the nvidia-driver in `nv_vulkan_wrapper.so` is correct).
3. Ensure that the (unwrapped) nvidia driver is not registered (e.g. in `/usr/share/vulkan/icd.d/nvidia_icd.json`) and create a similar file `nv_vulkan_wrapper.json` where the path to the driver points to the compiled `libnv_vulkan_wrapper.so`.
4. (Optional) Run `optirun primus_vk_diag`. It has to display entries for both graphics cards, otherwise the driver setup is broken. You can also test with `optirun vulkaninfo` that your Vulkan drivers are at least detecting your graphics cards.
5. Install `primus_vk.json` and adjust path.
6. Run `ENABLE_PRIMUS_LAYER=1 optirun vulkan-smoketest`.

### Arch Linux

Notes for running on Arch Linux:

* nv_vulkan_wrapper.cpp: Change nvDriver path to `/usr/lib/libGLX_nvidia.so.0`
* primus_vk.cpp: add: `#include "vk_layer_utils.h"` (on Debian the contents are included in some other header and there is no "vk_layer_utils.h")

### RPM package

Leonid Maksymchuk built RPM packaging scripts for primus-vk which can be found in his [repository](https://github.com/leonmaxx/primus-vk-rpm). RPMs for Fedora >= 30 are available [here](https://copr.fedorainfracloud.org/coprs/yura/primus-vk/)

## Credits

This layer is based on the sample layer available under https://github.com/baldurk/sample_layer. The guide that goes along with it is [https://renderdoc.org/vulkan-layer-guide.html](https://renderdoc.org/vulkan-layer-guide.html).
