# Builds hello.dylib / hello.dll / hello.so. Plugins only need the one header.
cmake_minimum_required(VERSION 3.20)
project(msc_hello_plugin C)

add_library(hello SHARED hello.c)
target_include_directories(hello PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
set_target_properties(hello PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
