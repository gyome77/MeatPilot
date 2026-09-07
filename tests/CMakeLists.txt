# doctest is vendored as a single header under third_party/.
#
# It was previously pulled with FetchContent, which contacts GitHub on every
# reconfigure. On a sandboxed or offline machine that blocks indefinitely: the
# build produces no output and appears to hang rather than failing, and every
# subsequent `cmake --build` queues behind it. Vendoring one 316 KB header
# makes the build deterministic, offline-capable and considerably faster.
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

add_executable(meatpilot_tests
  main.cpp
  unit/test_hysteresis.cpp
  unit/test_guards.cpp
  unit/test_coupling.cpp
  unit/test_ventilation.cpp
  unit/test_sensor.cpp
  unit/test_derived.cpp
  unit/test_product.cpp
  unit/test_profiles.cpp
  unit/test_program.cpp
  unit/test_alarm.cpp
  invariant/test_invariants.cpp
  scenario/test_acceptance.cpp
  scenario/test_closed_loop.cpp
  scenario/test_integration.cpp
)

target_link_libraries(meatpilot_tests PRIVATE meatpilot_sim doctest)
target_include_directories(meatpilot_tests PRIVATE .)

# doctest.h is large and every test translation unit includes it. Without a
# precompiled header a full rebuild of the suite takes minutes.
target_precompile_headers(meatpilot_tests PRIVATE <doctest/doctest.h>)

# main.cpp must NOT use the PCH: it defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
# before including doctest, and a prefix header would have included doctest
# already -- so the runner and main() would never be generated, and the link
# fails on a missing _main.
set_source_files_properties(main.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# One ctest entry running every case in a single process. Registering each case
# separately cost about a second of process launch apiece -- 100 seconds of
# overhead for 0.2 seconds of testing.
add_test(NAME meatpilot COMMAND meatpilot_tests)
