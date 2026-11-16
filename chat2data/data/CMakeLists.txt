set(BREEZEKIT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../libSource/cpp-service-kit" CACHE PATH "BreezeKit")
include("${BREEZEKIT_SOURCE_DIR}/cmake/dependencies/odb.cmake")

find_program(ODB_COMPILER NAMES odb)
if(NOT ODB_COMPILER)
    message(FATAL_ERROR "未找到 ODB 编译器")
endif()

set(ODB_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/odb")
set(SQL_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sql")
set(ENTITY_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/userEntity.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/sessionEntity.hpp"
)

foreach(ENTITY userEntity sessionEntity)
    add_custom_command(
        OUTPUT "${ODB_OUTPUT_DIR}/${ENTITY}-odb.cxx" "${ODB_OUTPUT_DIR}/${ENTITY}-odb.hxx" # 生成的 ODB 代码
        BYPRODUCTS "${ODB_OUTPUT_DIR}/${ENTITY}-odb.ixx" # 生成ixx文件
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ODB_OUTPUT_DIR}"
        COMMAND "${ODB_COMPILER}" -d mysql --std c++11 --generate-query
                --output-dir "${ODB_OUTPUT_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/${ENTITY}.hpp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${ENTITY}.hpp"
        VERBATIM
    )
endforeach()

# 两个映射共同生成一份建表脚本，不包含删表语句。
add_custom_command(
    OUTPUT "${SQL_OUTPUT_DIR}/create_tables.sql"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SQL_OUTPUT_DIR}"
    COMMAND "${ODB_COMPILER}" -d mysql --std c++11 --mysql-engine InnoDB --generate-schema-only --at-once
            --omit-drop --input-name create_tables --output-dir "${SQL_OUTPUT_DIR}" ${ENTITY_HEADERS}
    DEPENDS ${ENTITY_HEADERS}
    VERBATIM
)
add_custom_target(user_schema ALL DEPENDS "${SQL_OUTPUT_DIR}/create_tables.sql")

add_library(user_entities STATIC
    "${ODB_OUTPUT_DIR}/userEntity-odb.cxx"
    "${ODB_OUTPUT_DIR}/sessionEntity-odb.cxx"
)
target_compile_features(user_entities PUBLIC cxx_std_17)
set_target_properties(user_entities PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(user_entities PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${ODB_OUTPUT_DIR}")
target_link_libraries(user_entities PUBLIC BreezeDep::odb BreezeDep::odbmysql)
