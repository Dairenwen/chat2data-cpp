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
target_link_libraries(gateway_service PRIVATE gateway_breezekit)
