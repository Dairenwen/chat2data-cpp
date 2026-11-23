set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/code.cmake")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/rpc.cmake")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/etcd.cmake")

# 通知实现和测试共用邮件客户端与 RPC 代码，公共日志和工具由项目公共库提供。
add_library(notify_service_rpc STATIC
    notifyBusiness.cpp
    notifyServiceImpl.cpp
    notifyServer.cpp
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeCode.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeEtcd.cpp"
)
target_compile_features(notify_service_rpc PUBLIC cxx_std_17)
target_include_directories(notify_service_rpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(notify_service_rpc
    PUBLIC BreezeDep::brpc notify_service_proto CURL::libcurl chat2data_common
    PRIVATE gflags BreezeDep::leveldb OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB
            Threads::Threads ${CMAKE_DL_LIBS} etcd-cpp-api Boost::boost
)

# 默认指向本模块现有模板，运行时可用 --mail_html_path 覆盖。
set(notify_mail_template "${CMAKE_CURRENT_SOURCE_DIR}/verifyCodeMail.html")
add_executable(notify_service main.cpp)
target_compile_definitions(notify_service PRIVATE NOTIFY_DEFAULT_MAIL_TEMPLATE="${notify_mail_template}")
target_link_libraries(notify_service PRIVATE notify_service_rpc gflags)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(notify_service_test ../tests/svc_notifyService/notifyServiceImpl_test.cpp)
    target_compile_definitions(notify_service_test PRIVATE
        NOTIFY_TEST_TEMPLATE="${notify_mail_template}")
    target_link_libraries(notify_service_test PRIVATE notify_service_rpc GTest::gtest gflags)
    # CTest 运行回环测试，真实投递用例由操作者显式运行。
    add_test(NAME notify_service_test COMMAND notify_service_test --gtest_filter=NotifyServiceTest.*)
    set_tests_properties(notify_service_test PROPERTIES TIMEOUT 30)
endif()
