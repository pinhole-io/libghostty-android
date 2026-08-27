cmake_minimum_required(VERSION 3.22.1)
project(harn_ghostty CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(HARN_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/vendor)

add_library(ghostty-vt STATIC IMPORTED)
set_target_properties(ghostty-vt PROPERTIES
    IMPORTED_LOCATION ${HARN_VENDOR_DIR}/${ANDROID_ABI}/libghostty-vt.a)

add_library(harn_ghostty SHARED
    ghostty_jni.cpp
    pty_jni.cpp)

target_include_directories(harn_ghostty PRIVATE
    ${HARN_VENDOR_DIR}/include)

target_compile_definitions(harn_ghostty PRIVATE GHOSTTY_STATIC)
target_compile_options(harn_ghostty PRIVATE -Wall -Wextra -fvisibility=hidden)
target_link_options(harn_ghostty PRIVATE -Wl,-z,max-page-size=16384)

if(ANDROID_ABI STREQUAL "x86_64")
    # Zig general-dynamic TLS on x86_64 needs __tls_get_addr.
    # Bionic exports it from API 29+. Leave it unresolved at link time.
    target_link_options(harn_ghostty PRIVATE -Wl,-z,undefs)
endif()

target_link_libraries(harn_ghostty ghostty-vt log)
