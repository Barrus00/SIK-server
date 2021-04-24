cmake_minimum_required(VERSION 3.19)
project(ZAD_1_poprawa)

set(CMAKE_CXX_STANDARD 17)

macro (add_executable _name)
    _add_executable(${ARGV})
    if (TARGET ${_name})
        target_link_libraries(${_name} err)
    endif()
endmacro()

add_library(err err.cpp)

add_executable(server main.cpp)