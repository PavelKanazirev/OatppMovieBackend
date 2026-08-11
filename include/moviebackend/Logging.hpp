/**
 * @file Logging.hpp
 * @brief Thin, explicit wrapper over spdlog.
 *
 * ## Why spdlog (and what it was compared against)
 *
 * The requirement was: FOSS, well documented, cross platform (Windows +
 * Linux), usable header-only or easily vendored, with log levels configurable
 * at build time *and* at run time. Three realistic candidates:
 *
 *  * **glog (Google)** - battle tested, but its configuration is global flag
 *    based, it has no first-class compile-time level stripping, and its
 *    Windows story historically needed patches. Rejected.
 *
 *  * **Boost.Log** - powerful and already "in the box" if you use Boost.
 *    Rejected on two counts: this project deliberately restricts Boost to
 *    serialisation, and Boost.Log is a *compiled* library with a famously
 *    heavy template front end - a poor fit for "header-only or easily
 *    vendored".
 *
 *  * **spdlog** - chosen. It is header-only by default (a compiled mode is
 *    opt-in purely for build speed), it is a single self-contained directory
 *    that vendors trivially, Windows and Linux are both first-class, and it
 *    solves the build-time/run-time level requirement exactly:
 *      - `SPDLOG_ACTIVE_LEVEL` removes call sites at *compile* time, so a
 *        release build does not even evaluate the arguments of a trace call.
 *      - `spdlog::set_level()` filters at *run* time within whatever remains.
 *    Our CMakeLists sets SPDLOG_ACTIVE_LEVEL to TRACE for Debug builds and
 *    ERROR for everything else, which is precisely the "very verbose for
 *    debug, Error/Critical only for release" requirement.
 *
 * ## Why the MB_LOG_* macros instead of calling spdlog directly
 *
 * Two reasons, both practical rather than dogmatic:
 *  1. The compile-time stripping only happens if you go through the
 *     `SPDLOG_*` macros. Calling `spdlog::trace(...)` directly would keep the
 *     call in a release binary.
 *  2. It leaves exactly one file to touch if the logging backend ever
 *     changes.
 */
#ifndef MOVIEBACKEND_LOGGING_HPP
#define MOVIEBACKEND_LOGGING_HPP

#include <string>

// SPDLOG_ACTIVE_LEVEL must be defined before spdlog.h is included. The build
// script defines it per configuration; this fallback keeps the header usable
// if somebody includes it from a translation unit outside our CMake targets.
#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO
#endif

#include <spdlog/spdlog.h>

namespace moviebackend {

/** @brief Run-time logging configuration, filled in from config.json. */
struct LoggingConfig {
    /** One of: trace, debug, info, warn, error, critical, off. */
    std::string level = "info";

    /** Optional log file path. Empty means "console only". */
    std::string file;

    /** spdlog pattern string; see the spdlog docs for the placeholders. */
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v";
};

/**
 * @brief Install the global logger.
 *
 * Safe to call more than once - a second call replaces the previous logger.
 * If @p config.file is set, output is written to both the console and that
 * file.
 *
 * @param config the desired run-time configuration
 * @throws ServiceError with ErrorCode::InvalidArgument if the level name is
 *         not recognised.
 */
void initialiseLogging(const LoggingConfig& config);

/**
 * @brief Flush and release the global logger.
 *
 * Called once during shutdown. Matters for the Valgrind story: spdlog keeps
 * its registry in a function-local static, and dropping it explicitly keeps
 * the memcheck output free of "still reachable" noise.
 */
void shutdownLogging() noexcept;

} // namespace moviebackend

/* -------------------------------------------------------------------------
 * Logging macros.
 *
 * Usage is printf-like via the fmt syntax that spdlog embeds:
 *     MB_LOG_INFO("booked {} seats for showtime {}", seats.size(), id);
 * ------------------------------------------------------------------------- */
#define MB_LOG_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)
#define MB_LOG_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define MB_LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define MB_LOG_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define MB_LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define MB_LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

#endif // MOVIEBACKEND_LOGGING_HPP
