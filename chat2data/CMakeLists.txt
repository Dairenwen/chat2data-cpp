cmake_minimum_required(VERSION 3.16)

project(chat2data VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

add_library(chat2data_common STATIC
    common/errorHandler.cpp
    common/utils.cpp
)
target_compile_features(chat2data_common PUBLIC cxx_std_17)
set_target_properties(chat2data_common PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(chat2data_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/common")

if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(utils_test tests/common/utils_test.cpp)
    target_link_libraries(utils_test PRIVATE chat2data_common Threads::Threads)
    add_test(NAME utils_test COMMAND utils_test)
endif()

add_subdirectory(svc_gatewayService)
