find_package(Protobuf 3.12 REQUIRED)

set(PROTO_CODE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protoCode")
set(PROTO_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/userService.proto"
    "${CMAKE_CURRENT_SOURCE_DIR}/notifyService.proto"
)
set(PROTO_SOURCES
    "${PROTO_CODE_DIR}/userService.pb.cc"
    "${PROTO_CODE_DIR}/notifyService.pb.cc"
)
set(PROTO_HEADERS
    "${PROTO_CODE_DIR}/userService.pb.h"
    "${PROTO_CODE_DIR}/notifyService.pb.h"
)

add_custom_command(
    OUTPUT ${PROTO_SOURCES} ${PROTO_HEADERS}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROTO_CODE_DIR}"
    COMMAND protobuf::protoc
            "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}" "--cpp_out=${PROTO_CODE_DIR}" ${PROTO_FILES}
    DEPENDS protobuf::protoc ${PROTO_FILES}
    VERBATIM
)

add_library(user_service_proto STATIC ${PROTO_SOURCES} ${PROTO_HEADERS})
target_compile_features(user_service_proto PUBLIC cxx_std_17)
set_target_properties(user_service_proto PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(user_service_proto PUBLIC "${PROTO_CODE_DIR}")
target_link_libraries(user_service_proto PUBLIC protobuf::libprotobuf)
