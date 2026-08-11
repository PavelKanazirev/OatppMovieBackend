/**
 * @file AdminServiceTest.cpp
 * @brief Tests for the administrative facade.
 *
 * Covers each requirement from the "special user" list: append/modify/remove
 * theaters, append/modify/remove movies, the default timeline, and
 * append/modify/remove of individual time slots.
 */
#include "moviebackend/AdminService.hpp"

#include "moviebackend/Errors.hpp"
#include "TestSupport.hpp"

#include <gtest/gtest.h>

using moviebackend::AdminService;
using moviebackend::Catalog;
using moviebackend::CatalogSnapshot;
using moviebackend::ErrorCode;
using moviebackend::Movie;
using moviebackend::ScheduleConfig;
using moviebackend::Showtime;
using moviebackend::Theater;
using moviebackend::TheaterDefaults;
using moviebackend::testsupport::InMemoryCatalogStore;
using moviebackend::testsupport::makeMovie;
using moviebackend::testsupport::makeTheater;
namespace schedule = moviebackend::schedule;

namespace {

class AdminServiceTest : public ::testing::Test {
protected:
    AdminServiceTest()
        : m_defaults()
        , m_catalog(m_defaults)
        , m_store()
        , m_schedule()
        , m_service(m_catalog, m_store, m_schedule)
    {
    }

    void SetUp() override
    {
        m_catalog.addMovie(makeMovie("m1", "The Silent Protocol", 110));
        m_catalog.addTheater(makeTheater("t1", "Cinema Central", 20, 10));
    }

    TheaterDefaults      m_defaults;
    Catalog              m_catalog;
    InMemoryCatalogStore m_store;
    ScheduleConfig       m_schedule;   // 09:00 .. 21:00
    AdminService         m_service;
};

/* ---------------------------------------------------------------------------
 * Theaters: append / modify / remove
 * ------------------------------------------------------------------------- */

TEST_F(AdminServiceTest, AppendsATheater)
{
    const Theater added = m_service.addTheater(makeTheater("", "New Hall", 30, 10));

    EXPECT_FALSE(added.id.empty());
    EXPECT_EQ("New Hall", added.name);
    EXPECT_EQ(30, added.seatCapacity);
    EXPECT_EQ(2u, m_service.listTheaters().size());
}

TEST_F(AdminServiceTest, AppendedTheaterWithoutCapacityGetsTheConfiguredDefault)
{
    Theater theater;
    theater.name = "Default Sized";

    const Theater added = m_service.addTheater(theater);

    EXPECT_EQ(20, added.seatCapacity);   // the required default
    EXPECT_EQ(10, added.seatsPerRow);
}

TEST_F(AdminServiceTest, ModifiesATheater)
{
    const Theater updated =
        m_service.updateTheater("t1", makeTheater("t1", "Renamed", 40, 10));

    EXPECT_EQ("Renamed", updated.name);
    EXPECT_EQ(40, updated.seatCapacity);
    EXPECT_EQ(40, m_catalog.getTheater("t1").seatCapacity);
}

TEST_F(AdminServiceTest, RemovesATheater)
{
    m_service.removeTheater("t1");

    EXPECT_TRUE(m_service.listTheaters().empty());
    EXPECT_SERVICE_ERROR(m_service.removeTheater("t1"), ErrorCode::NotFound);
}

TEST_F(AdminServiceTest, InvalidTheaterEditsAreRejected)
{
    EXPECT_SERVICE_ERROR(m_service.addTheater(makeTheater("", "", 20, 10)),
                         ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(m_service.updateTheater("nope",
                                                 makeTheater("", "X", 20, 10)),
                         ErrorCode::NotFound);
}

/* ---------------------------------------------------------------------------
 * Movies: append / modify / remove
 * ------------------------------------------------------------------------- */

TEST_F(AdminServiceTest, AppendsModifiesAndRemovesAMovie)
{
    const Movie added = m_service.addMovie(makeMovie("", "Fresh Film", 100));
    EXPECT_FALSE(added.id.empty());
    EXPECT_EQ(2u, m_service.listMovies().size());

    const Movie updated =
        m_service.updateMovie(added.id, makeMovie(added.id, "Renamed Film", 120));
    EXPECT_EQ("Renamed Film", updated.title);
    EXPECT_EQ(120, updated.durationMinutes);

    m_service.removeMovie(added.id);
    EXPECT_EQ(1u, m_service.listMovies().size());
}

TEST_F(AdminServiceTest, AdminSeesUnscheduledMoviesThatUsersDoNot)
{
    // m1 has no showtimes yet, but the administrator must still see it in
    // order to schedule it.
    EXPECT_EQ(1u, m_service.listMovies().size());
    EXPECT_TRUE(m_catalog.listPlayingMovies().empty());
}

TEST_F(AdminServiceTest, ChangingADurationDoesNotDisturbExistingShowtimes)
{
    m_service.applyDefaultSchedule("m1", "t1");
    const std::size_t before = m_service.listShowtimes("m1", "t1").size();

    m_service.updateMovie("m1", makeMovie("m1", "The Silent Protocol", 200));

    // Documented behaviour: the timeline is left alone until the
    // administrator explicitly regenerates it.
    EXPECT_EQ(before, m_service.listShowtimes("m1", "t1").size());
}

/* ---------------------------------------------------------------------------
 * The default timeline
 * ------------------------------------------------------------------------- */

TEST_F(AdminServiceTest, DefaultScheduleFollowsTheNineToTwentyOneRule)
{
    const std::vector<Showtime> created = m_service.applyDefaultSchedule("m1", "t1");

    const std::vector<std::string> expected = {
        "09:00", "11:00", "13:00", "15:00", "17:00", "19:00", "21:00"
    };

    ASSERT_EQ(expected.size(), created.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], schedule::formatTimeOfDay(created[i].startTime));
    }
}

TEST_F(AdminServiceTest, DefaultScheduleHonoursACustomDayWindow)
{
    ScheduleConfig narrow;
    narrow.dayStart = 12 * 60;
    narrow.dayEnd = 16 * 60;

    Catalog catalog(m_defaults);
    InMemoryCatalogStore store;
    AdminService service(catalog, store, narrow);

    catalog.addMovie(makeMovie("m1", "Short", 90));
    catalog.addTheater(makeTheater("t1", "Hall", 20, 10));

    const std::vector<Showtime> created = service.applyDefaultSchedule("m1", "t1");

    ASSERT_EQ(3u, created.size());
    EXPECT_EQ("12:00", schedule::formatTimeOfDay(created[0].startTime));
    EXPECT_EQ("14:00", schedule::formatTimeOfDay(created[1].startTime));
    EXPECT_EQ("16:00", schedule::formatTimeOfDay(created[2].startTime));
}

TEST_F(AdminServiceTest, DefaultScheduleOnUnknownEntitiesIsNotFound)
{
    EXPECT_SERVICE_ERROR(m_service.applyDefaultSchedule("nope", "t1"),
                         ErrorCode::NotFound);
    EXPECT_SERVICE_ERROR(m_service.applyDefaultSchedule("m1", "nope"),
                         ErrorCode::NotFound);
}

/* ---------------------------------------------------------------------------
 * Individual time slots: append / modify / remove
 * ------------------------------------------------------------------------- */

TEST_F(AdminServiceTest, AppendsASingleTimeSlot)
{
    const Showtime added = m_service.addShowtime("m1", "t1", 14 * 60);

    EXPECT_FALSE(added.id.empty());
    EXPECT_EQ(14 * 60, added.startTime);
    EXPECT_EQ(20, added.seatCapacity);
    EXPECT_EQ(1u, m_service.listShowtimes("m1", "t1").size());
}

TEST_F(AdminServiceTest, ModifiesATimeSlot)
{
    const Showtime added = m_service.addShowtime("m1", "t1", 14 * 60);
    const Showtime moved = m_service.updateShowtime(added.id, 16 * 60);

    EXPECT_EQ(16 * 60, moved.startTime);
    EXPECT_EQ(16 * 60, m_catalog.getShowtime(added.id).startTime);
}

TEST_F(AdminServiceTest, RemovesATimeSlot)
{
    const Showtime added = m_service.addShowtime("m1", "t1", 14 * 60);

    m_service.removeShowtime(added.id);

    EXPECT_TRUE(m_service.listShowtimes("m1", "t1").empty());
    EXPECT_SERVICE_ERROR(m_service.removeShowtime(added.id), ErrorCode::NotFound);
}

TEST_F(AdminServiceTest, TimeSlotsOutsideTheConfiguredDayAreRejected)
{
    // The service-level policy, not a catalog invariant.
    EXPECT_SERVICE_ERROR(m_service.addShowtime("m1", "t1", 7 * 60),
                         ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(m_service.addShowtime("m1", "t1", 23 * 60),
                         ErrorCode::InvalidArgument);

    const Showtime added = m_service.addShowtime("m1", "t1", 14 * 60);
    EXPECT_SERVICE_ERROR(m_service.updateShowtime(added.id, 2 * 60),
                         ErrorCode::InvalidArgument);
}

TEST_F(AdminServiceTest, ClashingTimeSlotsAreRejected)
{
    m_service.addShowtime("m1", "t1", 14 * 60);

    EXPECT_SERVICE_ERROR(m_service.addShowtime("m1", "t1", 14 * 60),
                         ErrorCode::Conflict);
}

/* ---------------------------------------------------------------------------
 * Persistence control
 * ------------------------------------------------------------------------- */

TEST_F(AdminServiceTest, SaveWritesSynchronously)
{
    m_service.applyDefaultSchedule("m1", "t1");

    EXPECT_EQ(0u, m_store.saveCount());
    m_service.saveCatalog();
    EXPECT_EQ(1u, m_store.saveCount());

    const CatalogSnapshot stored = m_store.stored();
    EXPECT_EQ(1u, stored.movies.size());
    EXPECT_EQ(7u, stored.showtimes.size());
}

TEST_F(AdminServiceTest, ReloadReplacesInMemoryStateWithTheStoredOne)
{
    CatalogSnapshot seeded;
    seeded.movies.push_back(makeMovie("m9", "From Disk", 90));
    seeded.theaters.push_back(makeTheater("t9", "Disk Hall", 20, 10));
    m_store.seed(seeded);

    m_service.reloadCatalog();

    const std::vector<Movie> movies = m_service.listMovies();
    ASSERT_EQ(1u, movies.size());
    EXPECT_EQ("m9", movies[0].id);

    // The originally seeded m1/t1 are gone.
    EXPECT_SERVICE_ERROR(m_catalog.getMovie("m1"), ErrorCode::NotFound);
}

TEST_F(AdminServiceTest, ReloadingBrokenStoredDataLeavesTheCatalogIntact)
{
    CatalogSnapshot broken;
    moviebackend::ShowtimeSnapshot orphan;
    orphan.showtime.id = "s9";
    orphan.showtime.movieId = "ghost";
    orphan.showtime.theaterId = "ghost";
    orphan.showtime.startTime = 9 * 60;
    broken.showtimes.push_back(orphan);
    m_store.seed(broken);

    EXPECT_SERVICE_ERROR(m_service.reloadCatalog(), ErrorCode::InvalidArgument);

    // The live catalog must be untouched.
    EXPECT_NO_THROW(m_catalog.getMovie("m1"));
    EXPECT_EQ(1u, m_service.listTheaters().size());
}

TEST_F(AdminServiceTest, SaveFailureIsReportedToTheCaller)
{
    // Unlike the background worker, an explicit save reports the failure -
    // the administrator asked for a checkpoint and must be told it failed.
    m_store.failNextSave();
    EXPECT_SERVICE_ERROR(m_service.saveCatalog(), ErrorCode::PersistenceFailure);
}

} // unnamed namespace
