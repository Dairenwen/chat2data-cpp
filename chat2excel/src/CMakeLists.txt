cmake_minimum_required(VERSION 3.15)
project(Chat2ExcelServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
set(GFLAGS_USE_TARGET_NAMESPACE TRUE)
find_package(gflags REQUIRED)
find_path(httplibIncludeDirectory httplib.h)
if(NOT httplibIncludeDirectory)
    message(FATAL_ERROR "找不到 cpp-httplib 的 httplib.h，请安装或指定 httplibIncludeDirectory")
endif()

# 仅编译 bite_scaffold 的真实日志模块，避免服务器依赖无关的中间件。
get_filename_component(workspaceDirectory "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(libSourceDirectory "${workspaceDirectory}/libSource" CACHE PATH "依赖源码目录")
set(scaffoldSourceDirectory "${libSourceDirectory}/cpp-microservice-scaffold/source")
configure_file("${scaffoldSourceDirectory}/log.h"
    "${CMAKE_CURRENT_BINARY_DIR}/include/bite_scaffold/log.h" COPYONLY)

add_executable(Chat2ExcelServer
    Chat2ExcelServer.cc
    main.cc
    "${scaffoldSourceDirectory}/log.cc"
)
target_include_directories(Chat2ExcelServer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${httplibIncludeDirectory}"
)
target_link_libraries(Chat2ExcelServer PRIVATE
    Threads::Threads
    spdlog::spdlog
    fmt::fmt
    gflags::gflags
)
