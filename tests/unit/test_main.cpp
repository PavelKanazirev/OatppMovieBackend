/**
 * @file test_main.cpp
 * @brief Entry point for the unit test binary.
 *
 * A hand-written main() rather than linking gtest_main, for one reason: the
 * logger has to be configured before any test runs. Without this, every test
 * that exercises a code path containing an MB_LOG_* call would print through
 * spdlog's default logger and bury the test output.
 */
#include "moviebackend/Logging.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);

    // Quiet by default. Set MOVIEBACKEND_TEST_LOG_LEVEL=debug (or trace) when
    // a test is failing and the log would explain why.
    moviebackend::LoggingConfig logging;
    logging.level = "off";

    const char* override = std::getenv("MOVIEBACKEND_TEST_LOG_LEVEL");
    if (override != nullptr && *override != '\0') {
        logging.level = override;
    }

    moviebackend::initialiseLogging(logging);

    const int result = RUN_ALL_TESTS();

    moviebackend::shutdownLogging();
    return result;
}
