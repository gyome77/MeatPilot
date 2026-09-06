# Host-only simulated hardware. Never compiled into firmware; it exists so the
# acceptance suite can close the loop around the control engine on a laptop.
add_library(meatpilot_sim STATIC src/chamber.cpp)

target_include_directories(meatpilot_sim PUBLIC include)
target_link_libraries(meatpilot_sim PUBLIC meatpilot_core)
