cmake_minimum_required(VERSION 3.8)
project(rsync_client)

set(BOOST_ROOT "/Users/xiaohe/boost_1_64_0")
find_package(Boost 1.64.0 COMPONENTS system filesystem REQUIRED)
if(NOT Boost_FOUND)
    message(FATAL_ERROR "Could not find boost!")
endif()
message(STATUS "Boost_INCLUDE_DIRS: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost_LIBRARIES: ${Boost_LIBRARIES}")
message(STATUS "Boost_VERSION: ${Boost_VERSION}")
include_directories(${Boost_INCLUDE_DIRS})

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES rsync_client.cpp rsync_server.cpp)
add_executable(rsync_client ${SOURCE_FILES})