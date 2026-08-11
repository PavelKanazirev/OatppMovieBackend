/**
 * @file Config.cpp
 * @brief Loading config.json with Boost.JSON.
 */
#include "moviebackend/Config.hpp"

#include "moviebackend/Errors.hpp"
#include "moviebackend/Schedule.hpp"
#include "persistence/JsonHelpers.hpp"

#include <fstream>
#include <sstream>

namespace moviebackend {

namespace {

/**
 * @brief Read a whole file into a string.
 * @throws ServiceError InvalidArgument if the file cannot be opened.
 */
std::string readWholeFile(const std::string& path)
{
    std::ifstream stream(path.c_str(), std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        throw invalidArgument("cannot open '" + path + "' for reading");
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();

    if (stream.bad()) {
        throw invalidArgument("error while reading '" + path + "'");
    }
    return buffer.str();
}

} // unnamed namespace

Config Config::loadFromString(const std::string& json,
                              const std::string& originForErrors)
{
    using namespace jsonhelpers;

    Config config; // starts at the documented defaults

    const boost::json::value document = parseDocument(json, originForErrors);
    const boost::json::object& root = *requireObject(document, originForErrors, "<root>");

    /* ---- server ------------------------------------------------------- */
    const boost::json::object* server = optionalObject(root, "server", originForErrors, "server");
    if (server != nullptr) {
        config.server.host = optionalString(*server, "host", config.server.host,
                                            originForErrors, "server.host");

        const std::int64_t port = optionalInt(*server, "port", config.server.port,
                                              originForErrors, "server.port");
        // Port 0 is legitimate - it asks the OS for an ephemeral port, which
        // is how the smoke tests avoid colliding with a running instance.
        if (port < 0 || port > 65535) {
            throw invalidArgument(originForErrors
                                  + ": field 'server.port' must be 0..65535");
        }
        config.server.port = static_cast<std::uint16_t>(port);
    }

    /* ---- theater defaults --------------------------------------------- */
    const boost::json::object* theaterDefaults =
        optionalObject(root, "theater_defaults", originForErrors, "theater_defaults");
    if (theaterDefaults != nullptr) {
        config.theaterDefaults.seatCapacity = toInt(
            optionalInt(*theaterDefaults, "seat_capacity",
                        config.theaterDefaults.seatCapacity,
                        originForErrors, "theater_defaults.seat_capacity"),
            originForErrors, "theater_defaults.seat_capacity");

        config.theaterDefaults.seatsPerRow = toInt(
            optionalInt(*theaterDefaults, "seats_per_row",
                        config.theaterDefaults.seatsPerRow,
                        originForErrors, "theater_defaults.seats_per_row"),
            originForErrors, "theater_defaults.seats_per_row");

        if (config.theaterDefaults.seatCapacity <= 0) {
            throw invalidArgument(originForErrors
                                  + ": field 'theater_defaults.seat_capacity' must be positive");
        }
        if (config.theaterDefaults.seatsPerRow <= 0) {
            throw invalidArgument(originForErrors
                                  + ": field 'theater_defaults.seats_per_row' must be positive");
        }
    }

    /* ---- schedule ------------------------------------------------------ */
    const boost::json::object* scheduleObject =
        optionalObject(root, "schedule", originForErrors, "schedule");
    if (scheduleObject != nullptr) {
        const std::string dayStart =
            optionalString(*scheduleObject, "day_start",
                           schedule::formatTimeOfDay(config.schedule.dayStart),
                           originForErrors, "schedule.day_start");
        const std::string dayEnd =
            optionalString(*scheduleObject, "day_end",
                           schedule::formatTimeOfDay(config.schedule.dayEnd),
                           originForErrors, "schedule.day_end");

        config.schedule.dayStart = schedule::parseTimeOfDay(dayStart);
        config.schedule.dayEnd = schedule::parseTimeOfDay(dayEnd);

        if (config.schedule.dayEnd < config.schedule.dayStart) {
            throw invalidArgument(originForErrors
                                  + ": 'schedule.day_end' must not be before 'schedule.day_start'");
        }
    }

    /* ---- catalog ------------------------------------------------------- */
    const boost::json::object* catalog =
        optionalObject(root, "catalog", originForErrors, "catalog");
    if (catalog != nullptr) {
        config.catalog.path = optionalString(*catalog, "path", config.catalog.path,
                                             originForErrors, "catalog.path");
        config.catalog.autosave = optionalBool(*catalog, "autosave",
                                               config.catalog.autosave,
                                               originForErrors, "catalog.autosave");
        config.catalog.autosaveDebounceMs = toInt(
            optionalInt(*catalog, "autosave_debounce_ms",
                        config.catalog.autosaveDebounceMs,
                        originForErrors, "catalog.autosave_debounce_ms"),
            originForErrors, "catalog.autosave_debounce_ms");

        if (config.catalog.autosaveDebounceMs < 0) {
            throw invalidArgument(originForErrors
                                  + ": field 'catalog.autosave_debounce_ms' must not be negative");
        }
    }

    /* ---- logging ------------------------------------------------------- */
    const boost::json::object* logging =
        optionalObject(root, "logging", originForErrors, "logging");
    if (logging != nullptr) {
        config.logging.level = optionalString(*logging, "level", config.logging.level,
                                              originForErrors, "logging.level");
        config.logging.file = optionalString(*logging, "file", config.logging.file,
                                             originForErrors, "logging.file");
        config.logging.pattern = optionalString(*logging, "pattern",
                                                config.logging.pattern,
                                                originForErrors, "logging.pattern");
    }

    return config;
}

Config Config::loadFromFile(const std::string& path)
{
    return loadFromString(readWholeFile(path), path);
}

} // namespace moviebackend
