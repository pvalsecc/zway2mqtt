cmake_minimum_required(VERSION 2.8.9)
project(zway2mqtt)

set (CMAKE_C_FLAGS "-std=gnu99 -Werror -Wall")

set(Z-WAY /opt/z-way-server)
include_directories(${Z-WAY}/libzway)
link_directories(${Z-WAY}/libs)

set(SRCS main.c)

add_executable(zway2mqtt ${SRCS})
target_link_libraries(zway2mqtt mosquitto zway zcommons pthread xml2 z m crypto archive)