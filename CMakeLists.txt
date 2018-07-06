cmake_minimum_required (VERSION 3.8)
project (mandelbrot)

find_package(Vulkan)

set (CMAKE_CXX_STANDARD 14)

include_directories(${Vulkan_INCLUDE_DIR})

add_executable(mandelbrot src/mandelbrot.cc src/lodepng.cpp src/vulkan_ext.c)

target_link_libraries(mandelbrot ${Vulkan_LIBRARY})
