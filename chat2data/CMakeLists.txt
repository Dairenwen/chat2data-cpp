cmake_minimum_required(VERSION 3.16)

project(chat2data VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

set(BREEZEKIT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../libSource/cpp-service-kit" CACHE PATH "BreezeKit")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/redis.cmake")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/util.cmake")
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# 各模块在自己的构建目录中生成目标并注册测试，根 CTest 递归发现测试。
add_subdirectory(data)
add_subdirectory(proto)
add_subdirectory(common)
add_subdirectory(svc_userService)
add_subdirectory(svc_gatewayService)
add_subdirectory(svc_notifyService)
