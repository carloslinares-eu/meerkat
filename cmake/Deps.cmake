# Third-party dependencies, fetched at configure time. All are header-only
# (or used header-only), so nothing extra gets linked into the binary.
#
# Provides:
#   CLI11::CLI11                   - CLI11 2.5.0 (command-line parsing)
#   nlohmann_json::nlohmann_json   - nlohmann/json 3.12.0 (speed-test JSON)
#   tomlplusplus::tomlplusplus     - toml++ 3.4.0 (config file)
#   doctest::doctest               - doctest 2.5.3 (tests only)

include(FetchContent)

# --- CLI11 2.5.0 ---------------------------------------------------------------
set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG        v2.5.0
  GIT_SHALLOW    TRUE
  SYSTEM)
FetchContent_MakeAvailable(cli11)

# --- nlohmann/json 3.12.0 --------------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
FetchContent_MakeAvailable(nlohmann_json)

# --- toml++ 3.4.0 ----------------------------------------------------------------
FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG        v3.4.0
  GIT_SHALLOW    TRUE
  SYSTEM)
FetchContent_MakeAvailable(tomlplusplus)

# --- doctest 2.5.3 (tests only) -------------------------------------------------
if(MEERKAT_BUILD_TESTS)
  set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.5.3
    GIT_SHALLOW    TRUE
    SYSTEM)
  FetchContent_MakeAvailable(doctest)
endif()
