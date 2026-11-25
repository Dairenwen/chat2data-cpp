set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/fdfs.cmake")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/rpc.cmake")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/etcd.cmake")

get_filename_component(lib_source_dir "${BREEZEKIT_SOURCE_DIR}" DIRECTORY)
# 只构建 OpenXLSX 源码库，服务及测试使用同一个目标。
add_subdirectory("${lib_source_dir}/OpenXLSX/OpenXLSX" openxlsx EXCLUDE_FROM_ALL)

add_library(excel_parse_service_rpc STATIC
    excelColumn.cpp
    excelParser.cpp
    excelParserBusiness.cpp
    excelParserServiceImpl.cpp
    excelParserServer.cpp
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeFDFS.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeEtcd.cpp"
)
target_include_directories(excel_parse_service_rpc
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(excel_parse_service_rpc
    PUBLIC BreezeDep::brpc excel_parse_service_proto chat2data_common
    PRIVATE OpenXLSX::OpenXLSX BreezeDep::fdfsclient BreezeDep::fastcommon gflags
            BreezeDep::leveldb OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB
            Threads::Threads ${CMAKE_DL_LIBS} etcd-cpp-api Boost::boost
)

add_executable(excel_parse_service main.cpp)
target_link_libraries(excel_parse_service PRIVATE excel_parse_service_rpc gflags)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    set(excel_test_dir "${CMAKE_CURRENT_SOURCE_DIR}/../tests/svc_excelParserService")
    add_executable(excel_parser_test
        "${excel_test_dir}/excelParser_test.cpp"
        "${excel_test_dir}/excelParserServiceImpl_test.cpp"
    )
    target_compile_definitions(excel_parser_test PRIVATE EXCEL_TEST_FIXTURES="${excel_test_dir}/fixtures")
    target_link_libraries(excel_parser_test PRIVATE excel_parse_service_rpc OpenXLSX::OpenXLSX GTest::gtest_main)
    add_test(NAME excel_parser_test COMMAND excel_parser_test)
    set_tests_properties(excel_parser_test PROPERTIES TIMEOUT 30)
endif()
