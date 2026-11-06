cmake_minimum_required(VERSION 3.15)
project(Aw2AudioFix LANGUAGES C CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# 1. 引入 MinHook 子项目（利用其自带的 CMake 支持）
add_subdirectory(minhook)

# 2. 创建你的 Aw2AudioFix 动态库 (DLL) 项目
add_library(Aw2AudioFix MODULE
    Aw2AudioFix/dllmain.cpp
    Aw2AudioFix/ReadFileHook.cpp
    Aw2AudioFix/framework.h
)

# 3. 链接 MinHook 的头文件路径
target_include_directories(Aw2AudioFix PRIVATE
    minhook/include
)

# 4. 链接 MinHook 静态库
target_link_libraries(Aw2AudioFix PRIVATE minhook)

# 5. 针对 MSVC (Visual Studio) 的编译选项优化与警告消除
if(MSVC)
    target_compile_options(Aw2AudioFix PRIVATE /W3 /MD)
    # 强制输出名为 xaudio2_9redist.dll 的代理动态库
    set_target_properties(Aw2AudioFix PROPERTIES
        OUTPUT_NAME "xaudio2_9redist"
        PREFIX ""
    )
endif()