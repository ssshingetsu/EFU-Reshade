cmake_minimum_required(VERSION 3.20)
project(EndfieldUncensored LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT, no redist needed

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Endfield is 64-bit only: configure with -A x64")
endif()

add_library(minhook STATIC
  deps/minhook/src/buffer.c
  deps/minhook/src/hook.c
  deps/minhook/src/trampoline.c
  deps/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC deps/minhook/include)

add_library(EndfieldUncensored SHARED src/main.cpp)
target_include_directories(EndfieldUncensored PRIVATE deps/reshade deps/imgui)
target_link_libraries(EndfieldUncensored PRIVATE minhook)
set_target_properties(EndfieldUncensored PROPERTIES
  OUTPUT_NAME "EndfieldUncensored"
  SUFFIX ".addon64"
  PREFIX "")
