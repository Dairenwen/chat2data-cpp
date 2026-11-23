find_package(Protobuf 3.12 REQUIRED)

set(PROTO_CODE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protoCode")

# 每个协议单独生成和编译，调用方链接自己使用的协议库。
function(add_service_proto target name)
    set(proto_file "${CMAKE_CURRENT_SOURCE_DIR}/${name}.proto")
    set(proto_source "${PROTO_CODE_DIR}/${name}.pb.cc")
    set(proto_header "${PROTO_CODE_DIR}/${name}.pb.h")

    add_custom_command(
        OUTPUT "${proto_source}" "${proto_header}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROTO_CODE_DIR}"
        COMMAND protobuf::protoc
                "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}" "--cpp_out=${PROTO_CODE_DIR}" "${proto_file}"
        DEPENDS protobuf::protoc "${proto_file}"
        VERBATIM
    )

    add_library(${target} STATIC "${proto_source}" "${proto_header}")
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(${target} PUBLIC "${PROTO_CODE_DIR}")
    target_link_libraries(${target} PUBLIC protobuf::libprotobuf)
endfunction()

add_service_proto(user_service_proto userService)
add_service_proto(notify_service_proto notifyService)
