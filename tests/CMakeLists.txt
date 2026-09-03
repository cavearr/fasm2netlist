# Structural Verilog reader (flex/bison).  Only built when flex and bison are
# available -- see the top-level CMakeLists.
if(LVS_VERILOG_AVAILABLE)
  add_executable(verilog_parse_test verilog_parse_test.cpp)
  target_link_libraries(verilog_parse_test PRIVATE lvs_verilog)
  target_compile_options(verilog_parse_test PRIVATE -Wall -Wextra)
  add_test(NAME verilog_parse_test COMMAND verilog_parse_test)

  # Distributed RAM modelled as state: the properties that make a modelled
  # memory a memory -- hold, write, and leave the other bits alone -- each
  # proved over every address and every input rather than sampled at one.
  # Skipped (77) with no solver, like the other proof-based checks.
  add_executable(lvs_memstate_test lvs_memstate_test.cpp ../src/lvs/cone.cpp)
  target_link_libraries(lvs_memstate_test PRIVATE lvs_verilog lvs_miter)
  target_compile_options(lvs_memstate_test PRIVATE -Wall -Wextra)
  add_test(NAME lvs_memstate_test COMMAND lvs_memstate_test)
  set_tests_properties(lvs_memstate_test PROPERTIES SKIP_RETURN_CODE 77)
endif()

# Miter plumbing: the Boolean net, both export formats, and whichever
# solvers happen to be installed.  Skipped (77) when none is, since the
# solver is deliberately a command rather than a link-time dependency.
add_executable(lvs_selftest lvs_selftest.cpp)
target_link_libraries(lvs_selftest PRIVATE lvs_miter)
target_compile_options(lvs_selftest PRIVATE -Wall -Wextra)
add_test(NAME lvs_selftest COMMAND lvs_selftest)
set_tests_properties(lvs_selftest PROPERTIES SKIP_RETURN_CODE 77)

add_test(
  NAME smoke_test
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_smoke_test.sh
          $<TARGET_FILE:fasm2netlist>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
          ${CMAKE_CURRENT_BINARY_DIR}/smoke_test_out.v
)

# Real-world LVS/name-matching regression test against the VC707 Johnson
# counter example from the sibling xc7-bitstream-tools checkout (see
# tests/lvs/test_johnson_lvs.py). Skipped (not failed) if that external,
# un-vendored checkout/prjxray-db isn't present.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME johnson_lvs_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lvs/test_johnson_lvs.py
            --exe $<TARGET_FILE:fasm2netlist>
  )
  set_tests_properties(johnson_lvs_test PROPERTIES SKIP_RETURN_CODE 77)

  # Z3-based SOP equivalence proof: fasm2netlist's reconstruction (now with
  # --xdc/--part top-level IO) vs. the Johnson example's own gold Yosys
  # synthesis, every LUT converted to the same sum-of-products form on both
  # sides (see tests/lvs/prove_z3_sop_equiv.py). Skipped if the external
  # checkout or the z3-solver package isn't present.
  add_test(
    NAME johnson_z3_sop_equiv_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lvs/prove_z3_sop_equiv.py
            --exe $<TARGET_FILE:fasm2netlist>
  )
  set_tests_properties(johnson_z3_sop_equiv_test PROPERTIES SKIP_RETURN_CODE 77)

  # Oracle-free structural/SAT matcher: pairs fasm2netlist's anonymous FF
  # cells to the gold synthesis's own cells using ONLY cone-size/topology
  # signatures and SAT-miter falsification -- no placement.json fed to the
  # matcher itself (see tests/lvs/match_and_prove_sat.py); placement.json
  # is used solely afterward, to grade the oracle-free result against real
  # ground truth. Skipped if the external checkout or z3-solver isn't
  # present.
  add_test(
    NAME johnson_sat_match_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/lvs/match_and_prove_sat.py
            --exe $<TARGET_FILE:fasm2netlist>
  )
  set_tests_properties(johnson_sat_match_test PROPERTIES SKIP_RETURN_CODE 77)

  # RTL -> bitstream -> netlist, one design per cell family: yosys and
  # nextpnr-himbaechel build a bitstream from Verilog written here, and the
  # extraction is graded against nextpnr's own placement dump from that same
  # run (see tests/rtl/build_and_check.py). Skipped, not failed, when yosys or
  # a built nextpnr or the prjxray database isn't present -- none is vendored.
  add_test(
    NAME rtl_roundtrip_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/rtl/build_and_check.py
            --exe $<TARGET_FILE:fasm2netlist>
  )
  set_tests_properties(rtl_roundtrip_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
