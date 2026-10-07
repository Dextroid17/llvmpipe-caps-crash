#!/bin/sh
# Build the reproducer.  Needs Vulkan headers and libvulkan.
# On Arch/CachyOS the headers come from any Vulkan-Headers checkout; adjust INC.
INC="${VULKAN_HEADERS:-$HOME/Documents/AnyPS5/repo/3rdparty/Vulkan-Headers/include}"
g++ -std=c++20 -O0 -g main.cpp -o bda-repro -I"$INC" -lvulkan
