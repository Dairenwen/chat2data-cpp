cmake_minimum_required(VERSION 3.16)

project(chat2data VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

option(CHAT2DATA_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

add_library(chat2data_core STATIC src/application.cpp)
add_library(chat2data::core ALIAS chat2data_core)
target_compile_features(chat2data_core PUBLIC cxx_std_17)
set_target_properties(chat2data_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(chat2data_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(chat2data_core PRIVATE CHAT2DATA_VERSION="${PROJECT_VERSION}")

add_executable(chat2data src/main.cpp)
target_link_libraries(chat2data PRIVATE chat2data::core)

# Keep compiler policy local to this project, not its future dependencies.
function(chat2data_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(CHAT2DATA_ENABLE_SANITIZERS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT WIN32)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        else()
            message(FATAL_ERROR "Sanitizers require GCC or Clang on a supported non-Windows platform")
        endif()
    endif()
endfunction()

chat2data_configure_target(chat2data_core)
chat2data_configure_target(chat2data)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS chat2data RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
