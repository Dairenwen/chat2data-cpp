cmake_minimum_required(VERSION 3.15)
project(Chat2ExcelServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
set(GFLAGS_USE_TARGET_NAMESPACE TRUE)
find_package(gflags REQUIRED)
find_path(httplibIncludeDirectory httplib.h)
if(NOT httplibIncludeDirectory)
    message(FATAL_ERROR "找不到 cpp-httplib 的 httplib.h，请安装或指定 httplibIncludeDirectory")
endif()

# 编译 bite_scaffold 的日志、文件及 JSON 工具，避免引入无关中间件。
get_filename_component(workspaceDirectory "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(libSourceDirectory "${workspaceDirectory}/libSource" CACHE PATH "依赖源码目录")

# 与 ExcelParser 测试使用同一份 OpenXLSX 源码，保持解析行为和链接版本一致。
set(OPENXLSX_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
set(OPENXLSX_ENABLE_LTO OFF CACHE BOOL "" FORCE)
add_subdirectory("${libSourceDirectory}/OpenXLSX/OpenXLSX" openxlsx)

# 编译参考 ChatSDK；日志适配由 Chat2ExcelServer.cc 提供，统一使用 bite_scaffold 日志器。
# ChatSDK 内部引用所有云端提供者，因此保留其源文件；运行时仅配置 deepseek-chat。
set(chatSdkSourceDirectory "${libSourceDirectory}/AIModelAcessTech/sdk")
add_library(chatSdk STATIC
    "${chatSdkSourceDirectory}/src/ChatSDK.cpp"
    "${chatSdkSourceDirectory}/src/LLMManager.cpp"
    "${chatSdkSourceDirectory}/src/SessionManager.cpp"
    "${chatSdkSourceDirectory}/src/DataManager.cpp"
    "${chatSdkSourceDirectory}/src/DeepSeekProvider.cpp"
    "${chatSdkSourceDirectory}/src/ChatGPTProvider.cpp"
    "${chatSdkSourceDirectory}/src/GeminiProvider.cpp"
    "${chatSdkSourceDirectory}/src/OllamaLLMProvider.cpp"
)
target_include_directories(chatSdk PUBLIC
    "${chatSdkSourceDirectory}/include"
    "${httplibIncludeDirectory}"
)
target_compile_definitions(chatSdk PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(chatSdk PUBLIC
    JsonCpp::JsonCpp spdlog::spdlog fmt::fmt SQLite::SQLite3 OpenSSL::SSL OpenSSL::Crypto
)

set(scaffoldSourceDirectory "${libSourceDirectory}/cpp-microservice-scaffold/source")
configure_file("${scaffoldSourceDirectory}/log.h"
    "${CMAKE_CURRENT_BINARY_DIR}/include/bite_scaffold/log.h" COPYONLY)
configure_file("${scaffoldSourceDirectory}/util.h"
    "${CMAKE_CURRENT_BINARY_DIR}/include/bite_scaffold/util.h" COPYONLY)

add_executable(Chat2ExcelServer
    Chat2ExcelServer.cc
    ExeclParser.cc
    main.cc
    "${scaffoldSourceDirectory}/log.cc"
    "${scaffoldSourceDirectory}/util.cc"
)
target_include_directories(Chat2ExcelServer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/include"
    "${httplibIncludeDirectory}"
)
target_compile_definitions(Chat2ExcelServer PRIVATE
    CHAT2EXCEL_UPLOAD_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ExcelFile"
)
target_link_libraries(Chat2ExcelServer PRIVATE
    chatSdk
    OpenXLSX::OpenXLSX
    Threads::Threads
    spdlog::spdlog
    fmt::fmt
    JsonCpp::JsonCpp
    gflags::gflags
)
