/**
 * @file JsonCatalogStoreTest.cpp
 * @brief Tests for catalog serialisation and deserialisation.
 */
#include "moviebackend/CatalogStore.hpp"

#include "moviebackend/Catalog.hpp"
#include "moviebackend/Errors.hpp"
#include "TestSupport.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using moviebackend::Booking;
using moviebackend::Catalog;
using moviebackend::CatalogSnapshot;
using moviebackend::ErrorCode;
using moviebackend::JsonCatalogStore;
using moviebackend::ShowtimeSnapshot;
using moviebackend::TheaterDefaults;
using moviebackend::testsupport::makeMovie;
using moviebackend::testsupport::makeTheater;

namespace {

/** @brief Creates and removes a unique temporary directory per test. */
class JsonCatalogStoreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();

        m_directory = std::filesystem::temp_directory_path()
                      / ("moviebackend-test-" + std::string(info->name()));

        std::filesystem::remove_all(m_directory);
        std::filesystem::create_directories(m_directory);

        m_path = (m_directory / "catalog.json").string();
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_directory, ignored);
    }

    /** @brief A snapshot with one of everything. */
    static CatalogSnapshot makeSampleSnapshot()
    {
        CatalogSnapshot snapshot;
        snapshot.movies.push_back(makeMovie("m1", "The Silent Protocol", 110));
        snapshot.theaters.push_back(makeTheater("t1", "Cinema Central", 20, 10));

        ShowtimeSnapshot showtime;
        showtime.showtime.id = "s1";
        showtime.showtime.movieId = "m1";
        showtime.showtime.theaterId = "t1";
        showtime.showtime.startTime = 9 * 60;
        showtime.showtime.seatCapacity = 20;
        showtime.showtime.seatsPerRow = 10;
        showtime.bookedSeats.push_back("a1");
        showtime.bookedSeats.push_back("a2");
        snapshot.showtimes.push_back(showtime);

        Booking booking;
        booking.id = "b1";
        booking.showtimeId = "s1";
        booking.seats.push_back("a1");
        booking.seats.push_back("a2");
        booking.customerName = "Alice";
        booking.createdAtEpochSeconds = 1700000000;
        snapshot.bookings.push_back(booking);

        return snapshot;
    }

    std::filesystem::path m_directory;
    std::string           m_path;
    TheaterDefaults       m_defaults;
};

TEST_F(JsonCatalogStoreTest, RoundTripsThroughAFile)
{
    JsonCatalogStore store(m_path, m_defaults);
    const CatalogSnapshot original = makeSampleSnapshot();

    store.save(original);
    ASSERT_TRUE(std::filesystem::exists(m_path));

    const CatalogSnapshot loaded = store.load();

    ASSERT_EQ(1u, loaded.movies.size());
    EXPECT_EQ("m1", loaded.movies[0].id);
    EXPECT_EQ("The Silent Protocol", loaded.movies[0].title);
    EXPECT_EQ(110, loaded.movies[0].durationMinutes);

    ASSERT_EQ(1u, loaded.theaters.size());
    EXPECT_EQ(20, loaded.theaters[0].seatCapacity);
    EXPECT_EQ(10, loaded.theaters[0].seatsPerRow);

    ASSERT_EQ(1u, loaded.showtimes.size());
    EXPECT_EQ(9 * 60, loaded.showtimes[0].showtime.startTime);
    ASSERT_EQ(2u, loaded.showtimes[0].bookedSeats.size());
    EXPECT_EQ("a1", loaded.showtimes[0].bookedSeats[0]);

    ASSERT_EQ(1u, loaded.bookings.size());
    EXPECT_EQ("Alice", loaded.bookings[0].customerName);
    EXPECT_EQ(1700000000, loaded.bookings[0].createdAtEpochSeconds);
}

TEST_F(JsonCatalogStoreTest, NumbersSurviveTheRoundTripAsNumbers)
{
    // The concrete reason Boost.JSON was chosen over Boost.PropertyTree,
    // which would have written "20" as a string.
    const std::string text = JsonCatalogStore::serialise(makeSampleSnapshot());

    EXPECT_NE(std::string::npos, text.find("\"seat_capacity\": 20"));
    EXPECT_NE(std::string::npos, text.find("\"duration_minutes\": 110"));
    EXPECT_EQ(std::string::npos, text.find("\"seat_capacity\": \"20\""));
}

TEST_F(JsonCatalogStoreTest, TimesAreWrittenAsReadableStrings)
{
    const std::string text = JsonCatalogStore::serialise(makeSampleSnapshot());
    EXPECT_NE(std::string::npos, text.find("\"start_time\": \"09:00\""));
}

TEST_F(JsonCatalogStoreTest, MissingFileYieldsAnEmptyCatalogRatherThanAnError)
{
    // A first run on a clean checkout must boot, not fail.
    JsonCatalogStore store((m_directory / "does-not-exist.json").string(), m_defaults);

    CatalogSnapshot loaded;
    ASSERT_NO_THROW(loaded = store.load());

    EXPECT_TRUE(loaded.movies.empty());
    EXPECT_TRUE(loaded.theaters.empty());
    EXPECT_TRUE(loaded.showtimes.empty());
}

TEST_F(JsonCatalogStoreTest, TheaterEntriesMayOmitCapacityToGetTheDefault)
{
    const std::string json = R"({
        "movies":   [ { "id": "m1", "title": "T", "duration_minutes": 90 } ],
        "theaters": [ { "id": "t1", "name": "Defaults" } ]
    })";

    TheaterDefaults defaults;
    defaults.seatCapacity = 42;
    defaults.seatsPerRow = 7;

    const CatalogSnapshot loaded =
        JsonCatalogStore::parse(json, defaults, "<test>");

    ASSERT_EQ(1u, loaded.theaters.size());
    EXPECT_EQ(42, loaded.theaters[0].seatCapacity);
    EXPECT_EQ(7, loaded.theaters[0].seatsPerRow);
}

TEST_F(JsonCatalogStoreTest, RejectsMalformedDocuments)
{
    EXPECT_SERVICE_ERROR(JsonCatalogStore::parse("not json", m_defaults, "<test>"),
                         ErrorCode::InvalidArgument);

    EXPECT_SERVICE_ERROR(JsonCatalogStore::parse("[]", m_defaults, "<test>"),
                         ErrorCode::InvalidArgument);

    // movies must be an array
    EXPECT_SERVICE_ERROR(
        JsonCatalogStore::parse(R"({"movies": 5})", m_defaults, "<test>"),
        ErrorCode::InvalidArgument);

    // a movie without an id
    EXPECT_SERVICE_ERROR(
        JsonCatalogStore::parse(R"({"movies":[{"title":"T"}]})", m_defaults, "<test>"),
        ErrorCode::InvalidArgument);

    // a bad time string
    EXPECT_SERVICE_ERROR(
        JsonCatalogStore::parse(
            R"({"showtimes":[{"id":"s1","movie_id":"m1","theater_id":"t1",
                              "start_time":"9am"}]})",
            m_defaults, "<test>"),
        ErrorCode::InvalidArgument);
}

TEST_F(JsonCatalogStoreTest, ErrorMessagesPointAtTheOffendingEntry)
{
    try {
        JsonCatalogStore::parse(
            R"({"movies":[{"id":"m1","title":"A","duration_minutes":90},
                          {"id":"m2"}]})",
            m_defaults, "catalog.json");
        FAIL() << "expected a ServiceError";
    } catch (const moviebackend::ServiceError& error) {
        const std::string message = error.what();
        EXPECT_NE(std::string::npos, message.find("movies[1]"));
        EXPECT_NE(std::string::npos, message.find("title"));
    }
}

TEST_F(JsonCatalogStoreTest, SavingOverwritesAtomicallyAndLeavesNoTempFile)
{
    JsonCatalogStore store(m_path, m_defaults);

    store.save(makeSampleSnapshot());
    store.save(CatalogSnapshot());   // now save an empty catalog over it

    EXPECT_FALSE(std::filesystem::exists(m_path + ".tmp"));

    const CatalogSnapshot loaded = store.load();
    EXPECT_TRUE(loaded.movies.empty());
}

TEST_F(JsonCatalogStoreTest, SavingToAnUnwritableLocationReportsPersistenceFailure)
{
    JsonCatalogStore store((m_directory / "no" / "such" / "dir" / "c.json").string(),
                           m_defaults);

    EXPECT_SERVICE_ERROR(store.save(makeSampleSnapshot()),
                         ErrorCode::PersistenceFailure);
}

TEST_F(JsonCatalogStoreTest, ReadingAnUnreadableFileReportsPersistenceFailure)
{
    // A directory where a file is expected: exists() succeeds, opening fails.
    const std::string directoryAsPath = (m_directory / "a-directory").string();
    std::filesystem::create_directories(directoryAsPath);

    JsonCatalogStore store(directoryAsPath, m_defaults);
    EXPECT_SERVICE_ERROR(store.load(), ErrorCode::PersistenceFailure);
}

TEST_F(JsonCatalogStoreTest, SavedCatalogLoadsBackIntoALiveCatalog)
{
    // The end-to-end persistence path: catalog -> file -> catalog.
    TheaterDefaults defaults;
    Catalog source(defaults);
    source.addMovie(makeMovie("m1", "Round Trip", 100));
    source.addTheater(makeTheater("t1", "Hall", 20, 10));

    moviebackend::Showtime showtime;
    showtime.id = "s1";
    showtime.movieId = "m1";
    showtime.theaterId = "t1";
    showtime.startTime = 9 * 60;
    source.addShowtime(showtime);

    const Booking booking = source.bookSeats("s1", {"a1", "b3"}, "Alice");

    JsonCatalogStore store(m_path, defaults);
    store.save(source.snapshot());

    Catalog restored(defaults);
    restored.restore(store.load());

    EXPECT_EQ(18u, restored.listAvailableSeats("s1").size());
    EXPECT_EQ("Alice", restored.getBooking(booking.id).customerName);
    EXPECT_EQ(2u, restored.listBookedSeats("s1").size());
}

TEST_F(JsonCatalogStoreTest, HandWrittenCatalogFileIsAccepted)
{
    // A file a human would plausibly type, with fields omitted.
    const std::string json = R"({
      "movies": [
        { "id": "m1", "title": "Hand Written", "duration_minutes": 90 }
      ],
      "theaters": [
        { "id": "t1", "name": "Small Hall", "seat_capacity": 6, "seats_per_row": 3 }
      ],
      "showtimes": [
        { "id": "s1", "movie_id": "m1", "theater_id": "t1",
          "start_time": "18:00", "seat_capacity": 6, "seats_per_row": 3 }
      ]
    })";

    std::ofstream out(m_path.c_str());
    out << json;
    out.close();

    JsonCatalogStore store(m_path, m_defaults);

    TheaterDefaults defaults;
    Catalog catalog(defaults);
    ASSERT_NO_THROW(catalog.restore(store.load()));

    EXPECT_EQ(6u, catalog.listAvailableSeats("s1").size());
    EXPECT_EQ(18 * 60, catalog.getShowtime("s1").startTime);
}

} // unnamed namespace
