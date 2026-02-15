cmake_minimum_required(VERSION 3.20)
project(AttoClaw VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(CheckIPOSupported)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(CURL REQUIRED)

add_executable(attoclaw
  src/main.cpp
)

target_include_directories(attoclaw PRIVATE include)
target_link_libraries(attoclaw PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)

if(MSVC)
  target_compile_definitions(attoclaw PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_compile_options(attoclaw PRIVATE /W4 /O2)
  target_compile_options(attoclaw PRIVATE $<$<CONFIG:Release>:/GL>)
  target_link_options(attoclaw PRIVATE $<$<CONFIG:Release>:/LTCG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
else()
  target_compile_options(attoclaw PRIVATE -Wall -Wextra -Wpedantic -O3)
  
  # Optimization flags for GCC/Clang
  target_compile_options(attoclaw PRIVATE $<$<CONFIG:Release>:-ffunction-sections> $<$<CONFIG:Release>:-fdata-sections>)

  if(APPLE)
    # macOS specific linker flag for dead code stripping
    target_link_options(attoclaw PRIVATE $<$<CONFIG:Release>:-Wl,-dead_strip>)
  else()
    # Linux/GNU linker flag
    target_link_options(attoclaw PRIVATE $<$<CONFIG:Release>:-Wl,--gc-sections>)
  endif()
endif()

# Enable Link Time Optimization (LTO) if supported
check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
if(IPO_SUPPORTED)
  set_property(TARGET attoclaw PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(attoclaw_tests
    tests/tests.cpp
  )
  target_include_directories(attoclaw_tests PRIVATE include)
  target_link_libraries(attoclaw_tests PRIVATE nlohmann_json::nlohmann_json CURL::libcurl)
  add_test(NAME attoclaw_tests COMMAND attoclaw_tests)
endif()
