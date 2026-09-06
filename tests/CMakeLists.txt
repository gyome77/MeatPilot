include(FetchContent)

# doctest still declares cmake_minimum_required(VERSION 3.0), which CMake 4.x
# refuses outright. Raise the floor for this dependency only.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.12
)
FetchContent_MakeAvailable(doctest)

unset(CMAKE_POLICY_VERSION_MINIMUM)

add_executable(meatpilot_tests
  main.cpp
  unit/test_hysteresis.cpp
  unit/test_guards.cpp
  unit/test_coupling.cpp
  unit/test_ventilation.cpp
  unit/test_sensor.cpp
  unit/test_derived.cpp
  invariant/test_invariants.cpp
  scenario/test_acceptance.cpp
  scenario/test_closed_loop.cpp
)

target_link_libraries(meatpilot_tests PRIVATE meatpilot_sim doctest::doctest)
target_include_directories(meatpilot_tests PRIVATE .)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(meatpilot_tests)
