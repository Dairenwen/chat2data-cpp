cmake_minimum_required(VERSION 3.15)
project(ExcelParserTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

# 使用工作区内的 OpenXLSX 源码，避免测试和解析器链接不同版本。
get_filename_component(workspaceDirectory "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(libSourceDirectory "${workspaceDirectory}/libSource" CACHE PATH "依赖源码目录")
set(OPENXLSX_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
set(OPENXLSX_ENABLE_LTO OFF CACHE BOOL "" FORCE)
add_subdirectory("${libSourceDirectory}/OpenXLSX/OpenXLSX" openxlsx)

# 编译新版脚手架的日志模块，不引入其他中间件。
set(scaffoldSourceDirectory "${libSourceDirectory}/cpp-service-kit/source")
set(EXCEL_PARSER_TEST_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "测试 Excel 文件目录")

add_executable(ExcelParserTest
    ExcelParserTest.cc
    ../../src/ExeclParser.cc
    "${scaffoldSourceDirectory}/BreezeLog.cpp"
)
target_include_directories(ExcelParserTest PRIVATE
    ../../src
    "${scaffoldSourceDirectory}"
)
target_compile_definitions(ExcelParserTest PRIVATE
    EXCEL_PARSER_TEST_DIRECTORY="${EXCEL_PARSER_TEST_DIRECTORY}"
)
target_link_libraries(ExcelParserTest PRIVATE
    OpenXLSX::OpenXLSX
    GTest::gtest_main
    spdlog::spdlog
    fmt::fmt
    Threads::Threads
)

enable_testing()
# 整套测试运行于同一进程，共享一次生成的工作簿，避免并行写入同名文件。
add_test(NAME ExcelParserTest COMMAND ExcelParserTest)
