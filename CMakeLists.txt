cmake_minimum_required(VERSION 3.25)

project(meerkat
  VERSION 0.1.0
  DESCRIPTION "Monitors your internet connection and records evidence when it's slower than your contract"
  HOMEPAGE_URL "https://github.com/carloslinares-eu/meerkat"
  LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "meerkat supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MEERKAT_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(MEERKAT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(MEERKAT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

include(GNUInstallDirs)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Deps)
include(Warnings)

# --- generated version header -------------------------------------------------
configure_file(src/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/meerkat/version.hpp" @ONLY)

# --- core library: everything except main(), so tests can link it -------------
add_library(meerkat_lib STATIC
  src/app/cli.cpp
  src/app/check.cpp
  src/app/once.cpp
  src/app/settings.cpp
  src/core/clock.cpp
  src/core/config.cpp
  src/core/measurement.cpp
  src/core/outage.cpp
  src/core/scheduler.cpp
  src/core/status.cpp
  src/core/time.cpp
  src/net/interface.cpp
  src/net/link.cpp
  src/net/route.cpp
  src/net/wifi.cpp
  src/probe/dns.cpp
  src/probe/ookla.cpp
  src/probe/ping.cpp
  src/store/append_file.cpp
  src/store/csv.cpp
  src/store/manifest.cpp
  src/store/raw_log.cpp
  src/store/schema.cpp
  src/store/sha256.cpp
  src/sys/process.cpp
  src/sys/sleep_inhibitor.cpp
)
target_include_directories(meerkat_lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
find_package(Threads REQUIRED)
# libsystemd (sd-bus) for the logind sleep lock: Debian/Ubuntu `libsystemd-dev`.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
target_link_libraries(meerkat_lib PRIVATE PkgConfig::SYSTEMD)
target_link_libraries(meerkat_lib PUBLIC
  Threads::Threads
  CLI11::CLI11
  nlohmann_json::nlohmann_json
  tomlplusplus::tomlplusplus)
meerkat_set_warnings(meerkat_lib)

# --- executable ----------------------------------------------------------------
add_executable(meerkat src/main.cpp)
target_link_libraries(meerkat PRIVATE meerkat_lib)
meerkat_set_warnings(meerkat)
if(NOT MEERKAT_SANITIZE)
  # glibc stays dynamic; libstdc++/libgcc are baked into the shipped binary so
  # it runs on older Debian/Ubuntu releases too. Tests link normally.
  target_link_options(meerkat PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS meerkat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# --- tests ---------------------------------------------------------------------
if(MEERKAT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
