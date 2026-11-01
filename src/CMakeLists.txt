cmake_minimum_required(VERSION 3.20)
project(UnityEditorPreviewAgilityExt LANGUAGES CXX ASM_MASM)

# 产物名必须是 d3d12.dll（代理靠同名接管加载）
add_library(d3d12 SHARED
    d3d12_proxy.cpp
    thunks.asm
)

target_compile_features(d3d12 PRIVATE cxx_std_17)
target_compile_options(d3d12 PRIVATE /utf-8)
set_target_properties(d3d12 PROPERTIES
    OUTPUT_NAME d3d12
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 静态链接 CRT，避免目标机器缺 VC++ 运行库
if(MSVC)
    set_property(TARGET d3d12 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
