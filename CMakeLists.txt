cmake_minimum_required(VERSION 3.16)
project(fasm2netlist CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(fasm2netlist src/main.cpp)
target_compile_options(fasm2netlist PRIVATE -Wall -Wextra)

# LVS miter support: a small and-inverter graph plus exporters for the two
# formats a solver is likely to accept (DIMACS CNF, SMT-LIB 2) and a runner
# that shells out to one.  Kept a library of its own so the matcher being
# ported from tests/lvs/*.py can link it without going through main.cpp.
# Tile configuration: FASM -> the tile model's own config vocabulary, with a
# canonical dump so a model change is a diff.
add_library(lvs_tileconfig STATIC src/lvs/tileconfig.cpp)
target_compile_options(lvs_tileconfig PRIVATE -Wall -Wextra)
target_include_directories(lvs_tileconfig PUBLIC src)

add_executable(tileverilog src/lvs/tileverilog.cpp src/lvs/regmap.cpp)
target_link_libraries(tileverilog PRIVATE lvs_tileconfig)
target_include_directories(tileverilog PRIVATE src)
target_compile_options(tileverilog PRIVATE -Wall -Wextra)

add_executable(tiledump src/lvs/tiledump.cpp)
target_link_libraries(tiledump PRIVATE lvs_tileconfig)
target_compile_options(tiledump PRIVATE -Wall -Wextra)

add_library(lvs_miter STATIC src/lvs/boolnet.cpp src/lvs/solver.cpp)
target_compile_options(lvs_miter PRIVATE -Wall -Wextra)
target_include_directories(lvs_miter PUBLIC src)

# Structural Verilog reader, generated from src/lvs/verilog.{l,y}.  flex and
# bison are code generators, not link-time dependencies -- the sources they
# emit are self-contained C++ -- so this keeps the "no external libraries"
# property of the tool itself.  Without them installed the reader simply is
# not built, and what depends on it is skipped rather than failing.
find_package(BISON)
find_package(FLEX)
if(BISON_FOUND AND FLEX_FOUND)
  # bison writes the defines file itself and will not create the directory
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lvs)
  bison_target(VerilogParser src/lvs/verilog.y ${CMAKE_CURRENT_BINARY_DIR}/verilog.tab.cpp
               DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/lvs/verilog.tab.hpp)
  flex_target(VerilogLexer src/lvs/verilog.l ${CMAKE_CURRENT_BINARY_DIR}/verilog.yy.cpp)
  add_flex_bison_dependency(VerilogLexer VerilogParser)

  add_library(lvs_verilog STATIC
    ${BISON_VerilogParser_OUTPUTS}
    ${FLEX_VerilogLexer_OUTPUTS}
    src/lvs/verilog_reader.cpp)
  # two entries on purpose: the generated parser includes the defines file
  # by basename, while our own sources include it as lvs/verilog.tab.hpp
  target_include_directories(lvs_verilog PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}
                                                ${CMAKE_CURRENT_BINARY_DIR}/lvs)
  # generated sources are not ours to keep warning-clean
  target_compile_options(lvs_verilog PRIVATE -Wall -Wextra -Wno-unused-function)
  # Per-register equivalence: netlist -> cones -> miter -> whichever solver.
  add_executable(lvs_equiv src/lvs/lvs_equiv.cpp src/lvs/cone.cpp src/lvs/regmap.cpp)
  target_link_libraries(lvs_equiv PRIVATE lvs_verilog lvs_miter)
  target_compile_options(lvs_equiv PRIVATE -Wall -Wextra)

  set(LVS_VERILOG_AVAILABLE TRUE)
else()
  message(STATUS "flex/bison not found -- the structural Verilog reader will not be built")
  set(LVS_VERILOG_AVAILABLE FALSE)
endif()

install(TARGETS fasm2netlist RUNTIME DESTINATION bin)

enable_testing()
add_subdirectory(tests)
