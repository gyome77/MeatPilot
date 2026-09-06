# Host build. The ESP32 build is driven by ESP-IDF / PlatformIO and reuses the
# same core/ sources unchanged.
cmake_minimum_required(VERSION 3.20)
project(meatpilot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)

add_subdirectory(core)
add_subdirectory(platform/sim)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
