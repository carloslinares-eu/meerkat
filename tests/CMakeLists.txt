add_executable(meerkat_tests
  test_main.cpp
  test_cli.cpp
  test_process.cpp
)
target_link_libraries(meerkat_tests PRIVATE meerkat_lib doctest::doctest)
meerkat_set_warnings(meerkat_tests)

add_test(NAME meerkat_tests COMMAND meerkat_tests)
