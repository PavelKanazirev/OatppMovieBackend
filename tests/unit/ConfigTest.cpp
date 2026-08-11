/**
 * @file ConfigTest.cpp
 * @brief Tests for config.json parsing.
 *
 * The "capacity defaults to 20 but is configurable" requirement is a config
 * concern, so it is pinned down here.
 */
#include "moviebackend/Config.hpp"

#include "moviebackend/Errors.hpp"
#include "TestSupport.hpp"

#include <gtest/gtest.h>

using moviebackend::Config;
using moviebackend::ErrorCode;

namespace {

TEST(ConfigTest, EmptyDocumentYieldsDocumentedDefaults)
{
    const Config config = Config::loadFromString("{}", "<test>");

    // This is the requirement: 20 seats unless told otherwise.
    EXPECT_EQ(20, config.theaterDefaults.seatCapacity);
    EXPECT_EQ(10, config.theaterDefaults.seatsPerRow);

    EXPECT_EQ("0.0.0.0", config.server.host);
    EXPECT_EQ(8000, config.server.port);

    EXPECT_EQ(9 * 60, config.schedule.dayStart);
    EXPECT_EQ(21 * 60, config.schedule.dayEnd);

    EXPECT_EQ("config/catalog.json", config.catalog.path);
    EXPECT_TRUE(config.catalog.autosave);
    EXPECT_EQ(500, config.catalog.autosaveDebounceMs);

    EXPECT_EQ("info", config.logging.level);
}

TEST(ConfigTest, SeatCapacityIsConfigurable)
{
    const std::string json = R"({
        "theater_defaults": { "seat_capacity": 50, "seats_per_row": 25 }
    })";

    const Config config = Config::loadFromString(json, "<test>");

    EXPECT_EQ(50, config.theaterDefaults.seatCapacity);
    EXPECT_EQ(25, config.theaterDefaults.seatsPerRow);
}

TEST(ConfigTest, ReadsEverySection)
{
    const std::string json = R"({
        "server":  { "host": "127.0.0.1", "port": 9100 },
        "theater_defaults": { "seat_capacity": 30 },
        "schedule": { "day_start": "10:00", "day_end": "20:00" },
        "catalog": { "path": "/tmp/cat.json", "autosave": false,
                     "autosave_debounce_ms": 25 },
        "logging": { "level": "warn", "file": "/tmp/mb.log", "pattern": "%v" }
    })";

    const Config config = Config::loadFromString(json, "<test>");

    EXPECT_EQ("127.0.0.1", config.server.host);
    EXPECT_EQ(9100, config.server.port);

    // seats_per_row was omitted, so it keeps its default.
    EXPECT_EQ(30, config.theaterDefaults.seatCapacity);
    EXPECT_EQ(10, config.theaterDefaults.seatsPerRow);

    EXPECT_EQ(10 * 60, config.schedule.dayStart);
    EXPECT_EQ(20 * 60, config.schedule.dayEnd);

    EXPECT_EQ("/tmp/cat.json", config.catalog.path);
    EXPECT_FALSE(config.catalog.autosave);
    EXPECT_EQ(25, config.catalog.autosaveDebounceMs);

    EXPECT_EQ("warn", config.logging.level);
    EXPECT_EQ("/tmp/mb.log", config.logging.file);
    EXPECT_EQ("%v", config.logging.pattern);
}

TEST(ConfigTest, PortZeroIsAcceptedForEphemeralBinding)
{
    // The smoke tests rely on this to avoid clashing with a running server.
    const Config config = Config::loadFromString(R"({"server":{"port":0}})", "<test>");
    EXPECT_EQ(0, config.server.port);
}

TEST(ConfigTest, RejectsMalformedJson)
{
    EXPECT_SERVICE_ERROR(Config::loadFromString("{ not json", "<test>"),
                         ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(Config::loadFromString("[]", "<test>"),
                         ErrorCode::InvalidArgument);
}

TEST(ConfigTest, RejectsWrongTypesAndOutOfRangeValues)
{
    EXPECT_SERVICE_ERROR(
        Config::loadFromString(R"({"server":{"port":"8000"}})", "<test>"),
        ErrorCode::InvalidArgument);

    EXPECT_SERVICE_ERROR(
        Config::loadFromString(R"({"server":{"port":70000}})", "<test>"),
        ErrorCode::InvalidArgument);

    EXPECT_SERVICE_ERROR(
        Config::loadFromString(R"({"theater_defaults":{"seat_capacity":0}})", "<test>"),
        ErrorCode::InvalidArgument);

    EXPECT_SERVICE_ERROR(
        Config::loadFromString(R"({"theater_defaults":{"seat_capacity":-1}})", "<test>"),
        ErrorCode::InvalidArgument);

    EXPECT_SERVICE_ERROR(
        Config::loadFromString(R"({"catalog":{"autosave":"yes"}})", "<test>"),
        ErrorCode::InvalidArgument);

    EXPECT_SERVICE_ERROR(
        Config::loadFromString(R"({"catalog":{"autosave_debounce_ms":-1}})", "<test>"),
        ErrorCode::InvalidArgument);
}

TEST(ConfigTest, RejectsBadScheduleWindow)
{
    EXPECT_SERVICE_ERROR(
        Config::loadFromString(R"({"schedule":{"day_start":"9:00"}})", "<test>"),
        ErrorCode::InvalidArgument);

    EXPECT_SERVICE_ERROR(
        Config::loadFromString(
            R"({"schedule":{"day_start":"21:00","day_end":"09:00"}})", "<test>"),
        ErrorCode::InvalidArgument);
}

TEST(ConfigTest, ErrorMessageNamesTheOffendingField)
{
    // The whole reason for the JsonHelpers layer - a human editing the file
    // must be told which field is wrong.
    try {
        Config::loadFromString(R"({"server":{"port":"nope"}})", "my-config.json");
        FAIL() << "expected a ServiceError";
    } catch (const moviebackend::ServiceError& error) {
        const std::string message = error.what();
        EXPECT_NE(std::string::npos, message.find("my-config.json"));
        EXPECT_NE(std::string::npos, message.find("server.port"));
    }
}

TEST(ConfigTest, MissingFileIsReported)
{
    EXPECT_SERVICE_ERROR(
        Config::loadFromFile("/nonexistent/definitely/not/here.json"),
        ErrorCode::InvalidArgument);
}

TEST(ConfigTest, NullMeansAbsent)
{
    // "file": null and omitting "file" must behave identically.
    const Config config =
        Config::loadFromString(R"({"logging":{"file":null}})", "<test>");
    EXPECT_EQ("", config.logging.file);
}

} // unnamed namespace
