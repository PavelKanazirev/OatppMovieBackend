/**
 * @file Config.hpp
 * @brief Process configuration, loaded from a JSON file.
 *
 * Everything the exercise calls "configurable" lands here, most importantly
 * the default theater seat capacity (20 unless config.json says otherwise).
 */
#ifndef MOVIEBACKEND_CONFIG_HPP
#define MOVIEBACKEND_CONFIG_HPP

#include "moviebackend/Logging.hpp"
#include "moviebackend/Schedule.hpp"

#include <cstdint>
#include <string>

namespace moviebackend {

/** @brief Where the REST server listens. */
struct ServerConfig {
    std::string   host = "0.0.0.0";
    std::uint16_t port = 8000;
};

/** @brief Seating layout applied to a theater that does not specify its own. */
struct TheaterDefaults {
    /** The "defaults to 20 but must be configurable" requirement. */
    int seatCapacity = 20;

    /** Row width used to build seat labels (a1..a10, b1..b10, ...). */
    int seatsPerRow = 10;
};

/** @brief Catalog file location and autosave behaviour. */
struct CatalogConfig {
    /**
     * Path to the catalog JSON, relative to the process working directory
     * unless absolute. This is the file the admin API mutates at run time.
     */
    std::string path = "config/catalog.json";

    /**
     * When true a background thread writes the catalog back to @c path a
     * short while after every change. See PersistenceWorker.
     */
    bool autosave = true;

    /**
     * Debounce window in milliseconds. A burst of edits produces one write
     * rather than one write per edit.
     */
    int autosaveDebounceMs = 500;
};

/**
 * @brief The whole configuration of the process.
 *
 * Design pattern: none - this is a parameter object. It is loaded once at
 * startup, then passed by const reference and never mutated, which is why no
 * locking is needed around it.
 */
struct Config {
    ServerConfig    server;
    TheaterDefaults theaterDefaults;
    ScheduleConfig  schedule;
    CatalogConfig   catalog;
    LoggingConfig   logging;

    /**
     * @brief Parse a configuration file.
     *
     * Every field is optional; anything absent keeps the default shown in the
     * struct definitions above. That means an empty `{}` is a valid config
     * file and yields the documented defaults.
     *
     * @param path path to a JSON file
     * @return the parsed configuration
     * @throws ServiceError InvalidArgument if the file cannot be read, is not
     *         valid JSON, or contains a value of the wrong type / out of range.
     */
    static Config loadFromFile(const std::string& path);

    /**
     * @brief Parse a configuration from an in-memory JSON string.
     *
     * Exists so the unit tests can cover the parser without touching the
     * filesystem.
     *
     * @param json JSON document text
     * @param originForErrors label used in error messages (a filename, or
     *        something like "<test>")
     */
    static Config loadFromString(const std::string& json,
                                 const std::string& originForErrors);
};

} // namespace moviebackend

#endif // MOVIEBACKEND_CONFIG_HPP
