cmake_minimum_required(VERSION 3.14)
project(PxcLauncher VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# EUI-NEO 作为子工程编译其静态库 eui_neo（不构建它自带的示例/用户应用）。
# 依赖全部在 EUI-NEO-main/3rd 内置，离线可构建。
add_subdirectory(EUI-NEO-main)

# 我们的启动器 UI 程序
add_subdirectory(launcher)
