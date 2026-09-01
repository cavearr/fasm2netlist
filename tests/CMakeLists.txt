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
endif()
