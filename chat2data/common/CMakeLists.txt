add_library(chat2data_common STATIC
    errorHandler.cpp
    utils.cpp
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeUtil.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeLog.cpp"
)
target_compile_features(chat2data_common PUBLIC cxx_std_17)
set_target_properties(chat2data_common PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(chat2data_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(chat2data_common
    PUBLIC BreezeDep::redispp BreezeDep::hiredis JsonCpp::JsonCpp spdlog::spdlog Threads::Threads
    PRIVATE OpenSSL::Crypto)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(utils_test ../tests/common/utils_test.cpp)
    set_target_properties(utils_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(utils_test PRIVATE chat2data_common)
    add_test(NAME utils_test COMMAND utils_test)

    add_executable(credential_utils_test ../tests/common/credentialUtils_test.cpp)
    set_target_properties(credential_utils_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(credential_utils_test PRIVATE chat2data_common GTest::gtest_main)
    add_test(NAME credential_utils_test COMMAND credential_utils_test)

    # 缓存测试有独立程序，复用用户服务测试的连接参数和数据库准备逻辑。
    add_executable(cache_utils_test ../tests/common/cacheUtils_test.cpp)
    set_target_properties(cache_utils_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(cache_utils_test PRIVATE user_test_main)
    add_test(NAME cache_utils_test COMMAND cache_utils_test)
endif()
