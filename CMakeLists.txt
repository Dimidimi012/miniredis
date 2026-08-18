cmake_minimum_required(VERSION 3.10)
project(miniredis C)

# -std=c11 hides POSIX declarations; re-enable them explicitly.
add_definitions(-D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB MINIREDIS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_executable(miniredis ${MINIREDIS_SOURCES})
target_include_directories(miniredis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

enable_testing()

add_executable(test_util tests/test_util.c src/util.c)
add_executable(test_dict tests/test_dict.c src/dict.c src/util.c)
add_executable(test_resp tests/test_resp.c src/resp.c src/util.c)
add_executable(test_client tests/test_client.c)

foreach(t util dict resp)
    target_include_directories(test_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME test_${t} COMMAND test_${t})
endforeach()
