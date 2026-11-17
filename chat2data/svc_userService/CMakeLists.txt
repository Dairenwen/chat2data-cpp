# 复用项目已有的 Redis 依赖，并链接用户实体及 JSON 库。
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/redis.cmake")
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)

add_library(user_data STATIC userData.cpp sessionData.cpp verifyCodeData.cpp)
target_compile_features(user_data PUBLIC cxx_std_17)
set_target_properties(user_data PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(user_data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(user_data PUBLIC user_entities chat2data_common BreezeDep::redispp BreezeDep::hiredis JsonCpp::JsonCpp Threads::Threads spdlog::spdlog)

# 三个测试源文件共用一个测试程序及连接参数。
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)
    set(USER_DATA_TEST_ARGS "" CACHE STRING "用户数据访问测试的 gflags 参数列表")
    add_executable(user_data_test
        ../tests/common/cacheUtils_test.cpp
        ../tests/svc_userService/userData_test.cpp
        ../tests/svc_userService/sessionData_test.cpp
        ../tests/svc_userService/verifyCodeData_test.cpp
        "${BREEZEKIT_SOURCE_DIR}/source/BreezeODB.cpp"
        "${BREEZEKIT_SOURCE_DIR}/source/BreezeRedis.cpp"
    )
    target_compile_features(user_data_test PRIVATE cxx_std_17)
    set_target_properties(user_data_test PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(user_data_test PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
    target_compile_definitions(user_data_test PRIVATE USER_DATA_SCHEMA="${PROJECT_SOURCE_DIR}/data/sql/create_tables.sql")
    target_link_libraries(user_data_test PRIVATE user_data GTest::gtest gflags)
    add_dependencies(user_data_test user_schema)
    # CTest 通过参数列表传递连接信息；直接执行测试程序也可指定相同参数。
    if(USER_DATA_TEST_ARGS)
        gtest_discover_tests(user_data_test EXTRA_ARGS ${USER_DATA_TEST_ARGS} PROPERTIES TIMEOUT 30 RESOURCE_LOCK user_data_test)
    else()
        message(STATUS "未配置 USER_DATA_TEST_ARGS，可直接运行用户数据访问测试程序")
    endif()
endif()
