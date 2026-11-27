cmake_minimum_required(VERSION 3.24)
project(Retooled.DLSS.Native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Only the Vulkan headers are used. Unity supplies the loader at runtime. They come from the Vulkan SDK, or from a
# checkout of KhronosGroup/Vulkan-Headers whose include folder is passed as Vulkan_INCLUDE_DIR.
find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED)

# Streamline is loaded at runtime, so only its headers are needed here. Wintrust and Crypt32 check its signature.
add_library(DlssCore STATIC
    src/Dlss.cpp
    src/StreamlineLibrary.cpp
)
target_include_directories(DlssCore PUBLIC src PRIVATE streamline/include ${Vulkan_INCLUDE_DIR})
target_compile_definitions(DlssCore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES)
target_link_libraries(DlssCore PRIVATE wintrust crypt32)

# The C# assembly is Retooled.DLSS.dll, so the native plugin needs a name of its own.
add_library(DlssPlugin SHARED src/Plugin.cpp src/VulkanIntercept.cpp)
set_target_properties(DlssPlugin PROPERTIES OUTPUT_NAME Retooled.DLSS.Native)
target_include_directories(DlssPlugin PRIVATE unity ${Vulkan_INCLUDE_DIR})
target_compile_definitions(DlssPlugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES)
target_link_libraries(DlssPlugin PRIVATE DlssCore)

# Runs the same code on its own devices, without Unity. See test/Harness.cpp.
add_executable(DlssHarness test/Harness.cpp)
target_compile_definitions(DlssHarness PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(DlssHarness PRIVATE DlssCore d3d11 d3d12 dxgi)
