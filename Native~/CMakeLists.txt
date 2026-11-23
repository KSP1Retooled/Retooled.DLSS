cmake_minimum_required(VERSION 3.24)
project(RetooledDLSS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Streamline is loaded at runtime, so only its headers are needed here. Wintrust and Crypt32 check its signature.
add_library(DlssCore STATIC
    src/Dlss.cpp
    src/StreamlineLibrary.cpp
)
target_include_directories(DlssCore PUBLIC src PRIVATE streamline/include)
target_compile_definitions(DlssCore PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(DlssCore PRIVATE wintrust crypt32)

add_library(RetooledDLSS SHARED src/Plugin.cpp)
target_include_directories(RetooledDLSS PRIVATE unity)
target_compile_definitions(RetooledDLSS PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(RetooledDLSS PRIVATE DlssCore)

# Runs the same code on its own devices, without Unity. See test/Harness.cpp.
add_executable(DlssHarness test/Harness.cpp)
target_compile_definitions(DlssHarness PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(DlssHarness PRIVATE DlssCore d3d11 d3d12 dxgi)
