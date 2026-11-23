# 复用项目已有的 Redis 依赖，并链接用户实体及 JSON 库。
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/redis.cmake")
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)

add_library(user_data STATIC userData.cpp sessionData.cpp verifyCodeData.cpp)
target_compile_features(user_data PUBLIC cxx_std_17)
set_target_properties(user_data PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(user_data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(user_data PUBLIC user_entities chat2data_common BreezeDep::redispp BreezeDep::hiredis JsonCpp::JsonCpp Threads::Threads spdlog::spdlog)

include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/rpc.cmake")
add_library(user_business STATIC
    sessionManager.cpp
    userBusiness.cpp
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeRPC.cpp"
)
set_target_properties(user_business PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(user_business PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
# 用户业务通过通知协议发送验证码，协议库放在静态 brpc 之后链接。
target_link_libraries(user_business PUBLIC user_data PRIVATE
    BreezeDep::brpc notify_service_proto gflags BreezeDep::leveldb
    OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB ${CMAKE_DL_LIBS}
)

add_library(user_service_rpc STATIC userServiceImpl.cpp)
set_target_properties(user_service_rpc PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(user_service_rpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(user_service_rpc PUBLIC user_service_proto PRIVATE user_business BreezeDep::brpc)

# 用户服务入口复用 BreezeKit 的连接工厂、节点注册和服务发现。
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/etcd.cmake")
add_executable(user_service
    main.cpp
    userServer.cpp
    userServerBuilder.cpp
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeEtcd.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeODB.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeRedis.cpp"
)
set_target_properties(user_service PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(user_service PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(user_service PRIVATE
    user_service_rpc user_business BreezeDep::brpc gflags etcd-cpp-api Boost::boost
)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    # 各测试程序复用数据库连接代码和临时表结构。
    add_library(user_test_support STATIC
        "${BREEZEKIT_SOURCE_DIR}/source/BreezeODB.cpp"
        "${BREEZEKIT_SOURCE_DIR}/source/BreezeRedis.cpp"
    )
    set_target_properties(user_test_support PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(user_test_support PUBLIC "${BREEZEKIT_SOURCE_DIR}/source")
    target_compile_definitions(user_test_support INTERFACE USER_DATA_SCHEMA="${PROJECT_SOURCE_DIR}/data/sql/create_tables.sql")
    target_link_libraries(user_test_support PUBLIC user_data GTest::gtest gflags)
    add_dependencies(user_test_support user_schema)

    # 原测试入口和默认连接参数位于 userData_test.cpp。其他测试共用构建目录中生成的入口。
    set(USER_TEST_DIR "${PROJECT_SOURCE_DIR}/tests/svc_userService")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${USER_TEST_DIR}/userData_test.cpp")
    file(STRINGS "${USER_TEST_DIR}/userData_test.cpp" USER_TEST_FLAGS ENCODING UTF-8 REGEX "^DEFINE_")
    list(JOIN USER_TEST_FLAGS "\n" USER_TEST_FLAG_DEFINITIONS)
    set(USER_TEST_MAIN "${CMAKE_CURRENT_BINARY_DIR}/user_test_main.cpp")
    file(GENERATE OUTPUT "${USER_TEST_MAIN}" CONTENT
"#include <gflags/gflags.h>
#include <gtest/gtest.h>

${USER_TEST_FLAG_DEFINITIONS}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    return RUN_ALL_TESTS();
}
")
    add_library(user_test_main STATIC "${USER_TEST_MAIN}")
    set_target_properties(user_test_main PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(user_test_main PUBLIC user_test_support)

    # 每份测试源文件生成独立程序，CTest 使用程序内的 gflags 容器连接配置。
    function(add_user_service_test target source)
        add_executable(${target} "${USER_TEST_DIR}/${source}")
        set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
        target_link_libraries(${target} PRIVATE ${ARGN})
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    add_user_service_test(user_data_test userData_test.cpp
        user_test_support user_service_rpc user_business BreezeDep::brpc notify_service_proto)
    add_user_service_test(session_data_test sessionData_test.cpp user_test_main)
    add_user_service_test(session_manager_test sessionManager_test.cpp user_test_main user_business)
    add_user_service_test(user_service_impl_test userServiceImpl_test.cpp
        user_test_main user_service_rpc user_business BreezeDep::brpc notify_service_proto)
    add_user_service_test(verify_code_data_test verifyCodeData_test.cpp user_test_main)

    # 服务端集成测试启动实际的用户服务可执行文件。
    add_user_service_test(user_server_test userServer_test.cpp
        user_test_main user_business BreezeDep::brpc user_service_proto etcd-cpp-api)
    target_compile_definitions(user_server_test PRIVATE USER_SERVICE_EXECUTABLE="$<TARGET_FILE:user_service>")
    add_dependencies(user_server_test user_service)
    set_tests_properties(user_server_test PROPERTIES TIMEOUT 90)
endif()
