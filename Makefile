

libprimus_vk.so: primus_vk.cpp
	g++ -g3 -I/usr/include/vulkan -shared -fPIC -std=c++11 $^ -o $@

libnv_vulkan_wrapper.so: nv_vulkan_wrapper.cpp
	g++ -g3 -I/usr/include/vulkan -shared -fPIC -std=c++11 $^ -o $@

primus-vk-diag: primus_vk_diag.o
	g++ -g3 -o $@ $^ -lX11 -lvulkan -ldl

CXXFLAGS=-g3 -Wall -Werror
