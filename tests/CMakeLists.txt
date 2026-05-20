add_executable(test_interp_smoke test_interp_smoke.c)
target_link_libraries(test_interp_smoke PRIVATE gbjit_core)
add_test(NAME interp_smoke COMMAND test_interp_smoke)

add_executable(test_encoder_smoke test_encoder_smoke.c)
target_link_libraries(test_encoder_smoke PRIVATE gbjit_jit)
add_test(NAME encoder_smoke COMMAND test_encoder_smoke)

add_executable(test_encoder_bits test_encoder_bits.c)
target_link_libraries(test_encoder_bits PRIVATE gbjit_jit)
add_test(NAME encoder_bits COMMAND test_encoder_bits)

add_executable(test_xtensa_sim test_xtensa_sim.c)
target_link_libraries(test_xtensa_sim PRIVATE gbjit_jit)
add_test(NAME xtensa_sim COMMAND test_xtensa_sim)

add_executable(test_jit_differential test_jit_differential.c)
target_link_libraries(test_jit_differential PRIVATE gbjit_jit)
add_test(NAME jit_differential COMMAND test_jit_differential)

add_executable(test_smc test_smc.c)
target_link_libraries(test_smc PRIVATE gbjit_jit)
add_test(NAME smc COMMAND test_smc)

# Manual diagnostic tool (not registered as a ctest) — point at a .gb ROM.
add_executable(test_stepwise_diff test_stepwise_diff.c)
target_link_libraries(test_stepwise_diff PRIVATE gbjit_jit)

# Manual soak tool (not a ctest): runs a ROM through the dispatcher for a
# large cycle budget. Debug builds keep emit_xtensa.c's bounds asserts
# live. Used to exercise the cycle-counter wrap (~2^32 T-cycles).
add_executable(run_sml_host run_sml_host.c)
target_link_libraries(run_sml_host PRIVATE gbjit_jit)
