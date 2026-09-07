# core/ is pure C++17 with no platform dependencies. Nothing here may include
# an ESP-IDF, FreeRTOS or Arduino header -- tools/check-core-purity.sh enforces
# it in CI. That constraint is what lets the acceptance suite run on the host.
add_library(meatpilot_core STATIC
  src/control/engine.cpp
  src/control/names.cpp
  src/control/profiles.cpp
  src/product/registry.cpp
  src/program/programme.cpp
  src/program/runner.cpp
  src/program/presets.cpp
  src/alarm/condition.cpp
  src/alarm/manager.cpp
  src/sensor/conditioner.cpp
  src/sensor/derived.cpp
)

target_include_directories(meatpilot_core PUBLIC include)
target_compile_features(meatpilot_core PUBLIC cxx_std_17)
