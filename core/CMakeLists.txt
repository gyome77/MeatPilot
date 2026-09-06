# core/ is pure C++17 with no platform dependencies. Nothing here may include
# an ESP-IDF, FreeRTOS or Arduino header -- tools/check-core-purity.sh enforces
# it in CI. That constraint is what lets the acceptance suite run on the host.
add_library(meatpilot_core STATIC
  src/control/engine.cpp
  src/control/names.cpp
)

target_include_directories(meatpilot_core PUBLIC include)
target_compile_features(meatpilot_core PUBLIC cxx_std_17)
