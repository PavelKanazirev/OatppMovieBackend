/**
 * @file Logging.cpp
 * @brief Global logger setup.
 */
#include "moviebackend/Logging.hpp"

#include "moviebackend/Errors.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <vector>

namespace moviebackend {

namespace {

/**
 * @brief Translate a level name from config.json into spdlog's enum.
 * @throws ServiceError InvalidArgument for an unknown name.
 */
spdlog::level::level_enum parseLevel(const std::string& name)
{
    if (name == "trace")    { return spdlog::level::trace; }
    if (name == "debug")    { return spdlog::level::debug; }
    if (name == "info")     { return spdlog::level::info; }
    if (name == "warn")     { return spdlog::level::warn; }
    if (name == "warning")  { return spdlog::level::warn; }
    if (name == "error")    { return spdlog::level::err; }
    if (name == "critical") { return spdlog::level::critical; }
    if (name == "off")      { return spdlog::level::off; }

    throw invalidArgument(
        "unknown log level '" + name
        + "'; expected one of trace, debug, info, warn, error, critical, off");
}

} // unnamed namespace

void initialiseLogging(const LoggingConfig& config)
{
    // Validate before touching global state, so a bad level name leaves the
    // previous logger in place rather than a half-configured one.
    const spdlog::level::level_enum level = parseLevel(config.level);

    // spdlog's sinks are std::shared_ptr by API contract - the same
    // third-party constraint noted for oatpp in RestServer.hpp. It is
    // contained to this function.
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    if (!config.file.empty()) {
        // truncate = false: appending keeps the history of previous runs,
        // which is what you want when reproducing an intermittent bug.
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            config.file, false));
    }

    std::shared_ptr<spdlog::logger> logger =
        std::make_shared<spdlog::logger>("moviebackend", sinks.begin(), sinks.end());

    logger->set_pattern(config.pattern);
    logger->set_level(level);

    // Flush as soon as anything at warn or above is written: if the process
    // dies, the message explaining why must already be on disk.
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(logger);

    // The default logger's own level gates the MB_LOG_* macros at run time.
    // Note this is the *second* of the two filters: SPDLOG_ACTIVE_LEVEL has
    // already removed lower-severity call sites at compile time in a release
    // build, so setting "trace" here cannot resurrect them.
    spdlog::set_level(level);

    MB_LOG_INFO("logging initialised at level '{}'{}", config.level,
                config.file.empty() ? "" : (" -> " + config.file));
}

void shutdownLogging() noexcept
{
    // shutdown() flushes and drops every registered logger. Doing it
    // explicitly rather than leaving it to static destruction keeps the
    // Valgrind memcheck output clean - see cmake/Valgrind.cmake.
    spdlog::shutdown();
}

} // namespace moviebackend
