cmake_minimum_required(VERSION 3.20)
project(GatewayService LANGUAGES CXX)

set(BREEZEKIT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../libSource/cpp-service-kit" CACHE PATH "BreezeKit")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/rpc.cmake")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/etcd.cmake")
find_path(CPP_HTTPLIB_INCLUDE_DIR httplib.h)

add_library(gateway_breezekit STATIC
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeLog.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeUtil.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeRPC.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeEtcd.cpp"
)
target_compile_features(gateway_breezekit PUBLIC cxx_std_17)
set_target_properties(gateway_breezekit PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(gateway_breezekit PUBLIC "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(gateway_breezekit PUBLIC
    spdlog::spdlog Threads::Threads JsonCpp::JsonCpp Boost::boost
    BreezeDep::brpc protobuf::libprotobuf gflags BreezeDep::leveldb
    OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB ${CMAKE_DL_LIBS} etcd-cpp-api
)

add_executable(gateway_service
    main.cpp
    gatewayServer.cpp
    gatewayServiceImpl.cpp
)
target_compile_features(gateway_service PRIVATE cxx_std_17)
set_target_properties(gateway_service PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(gateway_service PRIVATE "${CPP_HTTPLIB_INCLUDE_DIR}")
# 网关挂载 wwwroot 下的产物目录
target_compile_definitions(gateway_service PRIVATE CHAT2DATA_WWWROOT="${CMAKE_CURRENT_SOURCE_DIR}/../wwwroot/dist")
target_link_libraries(gateway_service PRIVATE gateway_breezekit user_service_proto chat2data_common)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)

    add_executable(gateway_test ../tests/svc_gatewayService/gateway_test.cpp)
    target_compile_features(gateway_test PRIVATE cxx_std_17)
    set_target_properties(gateway_test PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(gateway_test PRIVATE "${CPP_HTTPLIB_INCLUDE_DIR}")
    target_compile_definitions(gateway_test PRIVATE GATEWAY_TEST_EXECUTABLE="$<TARGET_FILE:gateway_service>")
    target_link_libraries(gateway_test PRIVATE GTest::gtest_main etcd-cpp-api JsonCpp::JsonCpp Threads::Threads)
    add_dependencies(gateway_test gateway_service)
    # 网关默认创建 HTTP 工作线程池，串行运行进程级测试以控制资源占用
    gtest_discover_tests(gateway_test DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 90 RESOURCE_LOCK gateway_process)

    # 联合测试在网关模块运行，并复用用户测试的容器连接配置。
    add_executable(gateway_user_test ../tests/svc_gatewayService/gatewayUser_test.cpp)
    set_target_properties(gateway_user_test PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(gateway_user_test PRIVATE "${CPP_HTTPLIB_INCLUDE_DIR}")
    target_compile_definitions(gateway_user_test PRIVATE
        GATEWAY_TEST_EXECUTABLE="$<TARGET_FILE:gateway_service>"
        USER_SERVICE_EXECUTABLE="$<TARGET_FILE:user_service>")
    target_link_libraries(gateway_user_test PRIVATE user_test_main gateway_breezekit user_service_proto)
    add_dependencies(gateway_user_test gateway_service user_service)
    add_test(NAME gateway_user_test COMMAND gateway_user_test)
    set_tests_properties(gateway_user_test PROPERTIES TIMEOUT 60 RESOURCE_LOCK gateway_process)
endif()
