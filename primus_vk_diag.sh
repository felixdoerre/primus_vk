#!/bin/bash

export VK_LOADER_DEBUG=warn,error,info
export LD_DEBUG=libs
export ENABLE_PRIMUS_LAYER=0

function step_0 {
    printf "===== Round 0: Vulkaninfo =====\n"
    if which vulkaninfo >> /dev/null; then
	printf "==== Without Optirun ====\n"
	vulkaninfo 2>&1
	printf "==== With Optirun ====\n"
	optirun vulkaninfo 2>&1
    else
	printf "ERROR: Vulkaninfo is missing. Please install for more diagnostic data\n"
    fi
}
function step_1 {
    printf "===== Round 1: Plain Vulkan =====\n"
    ./primus_vk_diag vulkan 2>&1
}
function step_2 {
    printf "===== Round 2: Vulkan with optirun =====\n"
    optirun ./primus_vk_diag vulkan 2>&1
}
function step_3 {
    printf "===== Round 3: Vulkan with optirun and Primus layer =====\n"
    ENABLE_PRIMUS_LAYER=1 optirun ./primus_vk_diag vulkan 2>&1
}
function step_4 {
    printf "===== Round 4: Mixed Vulkan and OpenGL with Primus layer =====\n"
    ENABLE_PRIMUS_LAYER=1 optirun ./primus_vk_diag vulkan gl vulkan 2>&1
}
function step_5 {
    printf "===== Round 5: Mixed Vulkan and OpenGL with Primus layer while forcing primus-libGLa =====\n"
    ENABLE_PRIMUS_LAYER=1 optirun env PRIMUS_libGLa=/usr/lib/x86_64-linux-gnu/nvidia/current/libGLX_nvidia.so.0 ./primus_vk_diag vulkan gl vulkan 2>&1
}

if [[ $# == 0 ]]; then
    step_0
    step_1
    step_2
    step_3
    step_4
    step_5
else
    for arg in "$@"; do
	if [[ $arg == [0-5] ]]; then
	    step_$arg
	else
	    printf "Invalid argument\n" >&2
	fi
    done
fi
