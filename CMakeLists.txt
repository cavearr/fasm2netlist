cmake_minimum_required(VERSION 3.16)
project(fasm2netlist CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(fasm2netlist src/main.cpp)
target_compile_options(fasm2netlist PRIVATE -Wall -Wextra)

install(TARGETS fasm2netlist RUNTIME DESTINATION bin)

enable_testing()
add_subdirectory(tests)
