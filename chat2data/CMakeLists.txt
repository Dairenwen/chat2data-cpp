cmake_minimum_required(VERSION 3.16)

project(chat2data VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

set(BREEZEKIT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../libSource/cpp-service-kit" CACHE PATH "BreezeKit")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/redis.cmake")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/util.cmake")
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(chat2data_common STATIC
    common/errorHandler.cpp
    common/utils.cpp
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeUtil.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeLog.cpp"
)
target_compile_features(chat2data_common PUBLIC cxx_std_17)
set_target_properties(chat2data_common PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(chat2data_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/common"
    PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(chat2data_common PUBLIC BreezeDep::redispp BreezeDep::hiredis JsonCpp::JsonCpp spdlog::spdlog Threads::Threads)
target_link_libraries(chat2data_common PRIVATE OpenSSL::Crypto)

if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(utils_test tests/common/utils_test.cpp)
    target_link_libraries(utils_test PRIVATE chat2data_common Threads::Threads)
    add_test(NAME utils_test COMMAND utils_test)

    find_package(GTest REQUIRED)
    add_executable(credential_utils_test tests/common/credentialUtils_test.cpp)
    set_target_properties(credential_utils_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(credential_utils_test PRIVATE chat2data_common GTest::gtest_main)
    add_test(NAME credential_utils_test COMMAND credential_utils_test)
endif()

add_subdirectory(data)
add_subdirectory(proto)
add_subdirectory(svc_userService)
add_subdirectory(svc_gatewayService)
