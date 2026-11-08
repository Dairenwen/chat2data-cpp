cmake_minimum_required(VERSION 3.16)
project(Chat2ExcelServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
set(GFLAGS_USE_TARGET_NAMESPACE TRUE)
find_package(gflags REQUIRED)
# 使用工作区中的依赖源码。
get_filename_component(workspaceDirectory "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(libSourceDirectory "${workspaceDirectory}/libSource" CACHE PATH "依赖源码目录")

# 与 ExcelParser 测试使用同一份 OpenXLSX 源码，保持解析行为和链接版本一致。
set(OPENXLSX_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
set(OPENXLSX_ENABLE_LTO OFF CACHE BOOL "" FORCE)
add_subdirectory("${libSourceDirectory}/OpenXLSX/OpenXLSX" openxlsx)

# 使用 ModelBridge 发布包内同一版本的 cpp-httplib，避免系统旧头文件接口不兼容。
set(modelBridgeArchive "${libSourceDirectory}/ModelBridge/Release/ModelBridge-source.tar.gz")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${modelBridgeArchive}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${modelBridgeArchive}"
            ModelBridge-source/third_party/cpp-httplib/httplib.h
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RESULT_VARIABLE extractResult
)
if(NOT extractResult EQUAL 0)
    message(FATAL_ERROR "无法从 ModelBridge 发布包提取 cpp-httplib 头文件")
endif()
set(HTTPLIB_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ModelBridge-source/third_party/cpp-httplib")

# 使用 SDK 自带的 CMake 配置生成数据库 schema，只构建应用需要的 SDK 静态库。
function(addModelBridge)
    set(BUILD_TESTING OFF)
    add_subdirectory("${libSourceDirectory}/ModelBridge/SDK" modelbridge EXCLUDE_FROM_ALL)
endfunction()
addModelBridge()
# Ubuntu 22.04 的 JsonCpp 配置重复加载时不检查既有 target，先让 SDK 在其目录加载。
find_package(jsoncpp CONFIG REQUIRED)

set(scaffoldSourceDirectory "${libSourceDirectory}/cpp-service-kit/source")

add_executable(Chat2ExcelServer
    Chat2ExcelServer.cc
    ExeclParser.cc
    main.cc
    "${scaffoldSourceDirectory}/BreezeLog.cpp"
    "${scaffoldSourceDirectory}/BreezeUtil.cpp"
)
target_include_directories(Chat2ExcelServer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${scaffoldSourceDirectory}"
    "${HTTPLIB_INCLUDE_DIR}"
)
target_compile_definitions(Chat2ExcelServer PRIVATE
    CHAT2EXCEL_UPLOAD_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/ExcelFile"
    CHAT2EXCEL_WEB_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/rootwww"
    CPPHTTPLIB_OPENSSL_SUPPORT
)
target_link_libraries(Chat2ExcelServer PRIVATE
    ModelBridge::modelbridge_sdk
    OpenXLSX::OpenXLSX
    Threads::Threads
    spdlog::spdlog
    fmt::fmt
    JsonCpp::JsonCpp
    gflags::gflags
)
