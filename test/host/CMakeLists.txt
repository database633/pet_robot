cmake_minimum_required(VERSION 3.16)
project(mibot_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MIBOT_BOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/../../main/boards/mibot)

add_executable(test_mibot
    test_main.cpp
    test_frame_codec.cpp
    ${MIBOT_BOARD_DIR}/mibot_frame_codec.cc
)
target_include_directories(test_mibot PRIVATE ${MIBOT_BOARD_DIR} ${CMAKE_CURRENT_LIST_DIR})

enable_testing()
add_test(NAME mibot COMMAND test_mibot)
