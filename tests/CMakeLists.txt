add_executable(meerkat_tests
  test_main.cpp
  test_cli.cpp
  test_interface.cpp
  test_dns.cpp
  test_link.cpp
  test_measurement.cpp
  test_outage.cpp
  test_once.cpp
  test_ookla.cpp
  test_ping.cpp
  test_process.cpp
  test_route.cpp
  test_check.cpp
  test_config.cpp
  test_csv.cpp
  test_schema.cpp
  test_scheduler.cpp
  test_sha256.cpp
  test_sleep_inhibitor.cpp
  test_raw_log.cpp
  test_manifest.cpp
  test_status.cpp
  test_time.cpp
  test_wifi.cpp
)
target_link_libraries(meerkat_tests PRIVATE meerkat_lib doctest::doctest)
target_compile_definitions(meerkat_tests PRIVATE
  MEERKAT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
meerkat_set_warnings(meerkat_tests)

add_test(NAME meerkat_tests COMMAND meerkat_tests)
