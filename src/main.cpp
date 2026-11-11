/**
 * @file main.cpp
 * @brief Program entry point; forwards to meerkat::app::run().
 */
#include <cstddef>
#include <iostream>
#include <span>

#include "app/cli.hpp"

/**
 * @brief Program entry point.
 * @param argc Number of arguments.
 * @param argv The arguments.
 * @return The exit code from meerkat::app::run().
 */
int main(int argc, char* argv[]) {
    return meerkat::app::run(std::span<const char* const>{argv, static_cast<std::size_t>(argc)},
                             std::cout, std::cerr);
}
