include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/fdfs.cmake")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/rpc.cmake")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/etcd.cmake")

add_library(file_data STATIC fileData.cpp)
set_target_properties(file_data PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(file_data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(file_data PUBLIC file_entities chat2data_common)

add_library(file_business STATIC
    fileBusiness.cpp
    fileDependencies.cpp
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeFDFS.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeRPC.cpp"
)
set_target_properties(file_business PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(file_business PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(file_business PUBLIC file_data BreezeDep::brpc file_service_proto excel_parse_service_proto
    PRIVATE BreezeDep::fdfsclient BreezeDep::fastcommon gflags BreezeDep::leveldb OpenSSL::SSL OpenSSL::Crypto
            ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS})

add_library(file_service_rpc STATIC fileServiceImpl.cpp)
set_target_properties(file_service_rpc PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(file_service_rpc PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(file_service_rpc PUBLIC file_business)

add_executable(file_service
    main.cpp
    fileServer.cpp
    fileServerBuilder.cpp
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeEtcd.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeODB.cpp"
    "${BREEZEKIT_SOURCE_DIR}/source/BreezeRedis.cpp"
)
set_target_properties(file_service PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(file_service PRIVATE "${BREEZEKIT_SOURCE_DIR}/source")
target_link_libraries(file_service PRIVATE file_service_rpc BreezeDep::fdfsclient BreezeDep::fastcommon
    gflags etcd-cpp-api Boost::boost)

if(BUILD_TESTING)
    add_executable(file_service_test "${PROJECT_SOURCE_DIR}/tests/svc_fileService/fileService_test.cpp")
    set_target_properties(file_service_test PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_definitions(file_service_test PRIVATE
        FILE_SCHEMA="${PROJECT_SOURCE_DIR}/data/sql/create_file_tables.sql"
        FILE_WORKBOOK="${PROJECT_SOURCE_DIR}/tests/svc_excelParserService/fixtures/multipleWorksheets.xlsx")
    # 沿用开发容器的 MySQL、Redis 测试连接参数，文件测试使用独立数据库。
    target_link_libraries(file_service_test PRIVATE file_service_rpc user_test_main excel_parse_service_rpc)
    add_test(NAME file_service_test COMMAND file_service_test)
    set_tests_properties(file_service_test PROPERTIES TIMEOUT 60)
endif()
