add_test(
  NAME smoke_test
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_smoke_test.sh
          $<TARGET_FILE:fasm2netlist>
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
          ${CMAKE_CURRENT_BINARY_DIR}/smoke_test_out.v
)
