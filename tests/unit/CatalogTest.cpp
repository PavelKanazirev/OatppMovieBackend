/**
 * @file CatalogTest.cpp
 * @brief Single-threaded behaviour of the catalog.
 *
 * The concurrent behaviour - the part the exercise is really about - lives in
 * CatalogConcurrencyTest.cpp.
 */
#include "moviebackend/Catalog.hpp"

#include "moviebackend/Errors.hpp"
#include "moviebackend/Schedule.hpp"
#include "TestSupport.hpp"

#include <gtest/gtest.h>

using moviebackend::Booking;
using moviebackend::Catalog;
using moviebackend::CatalogSnapshot;
using moviebackend::ErrorCode;
using moviebackend::Movie;
using moviebackend::SeatLabel;
using moviebackend::Showtime;
using moviebackend::ShowtimeSnapshot;
using moviebackend::Theater;
using moviebackend::TheaterDefaults;
using moviebackend::testsupport::contains;
using moviebackend::testsupport::makeMovie;
using moviebackend::testsupport::makeTheater;

namespace {

/**
 * @brief A catalog pre-loaded with two movies, two theaters and showtimes.
 *
 * Design pattern: **Test Fixture**. Every test starts from the same known
 * world so that the assertions read as statements about behaviour rather than
 * about setup.
 */
class CatalogTest : public ::testing::Test {
protected:
    CatalogTest()
        : m_defaults()
        , m_catalog(m_defaults)
    {
    }

    void SetUp() override
    {
        m_catalog.addMovie(makeMovie("m1", "The Silent Protocol", 110));
        m_catalog.addMovie(makeMovie("m2", "Autobahn", 95));

        m_catalog.addTheater(makeTheater("t1", "Cinema Central", 20, 10));
        m_catalog.addTheater(makeTheater("t2", "Riverside Plex", 12, 6));

        Showtime showtime;
        showtime.id = "s1";
        showtime.movieId = "m1";
        showtime.theaterId = "t1";
        showtime.startTime = 9 * 60;
        m_catalog.addShowtime(showtime);

        showtime.id = "s2";
        showtime.startTime = 11 * 60;
        m_catalog.addShowtime(showtime);

        showtime.id = "s3";
        showtime.movieId = "m1";
        showtime.theaterId = "t2";
        showtime.startTime = 13 * 60;
        m_catalog.addShowtime(showtime);
    }

    TheaterDefaults m_defaults;   // 20 seats, 10 per row
    Catalog         m_catalog;
};

/* ---------------------------------------------------------------------------
 * The end-user journey
 * ------------------------------------------------------------------------- */

TEST_F(CatalogTest, ListsOnlyMoviesThatAreActuallyScheduled)
{
    // m2 exists but has no showtime, so it is not "playing".
    const std::vector<Movie> playing = m_catalog.listPlayingMovies();

    ASSERT_EQ(1u, playing.size());
    EXPECT_EQ("m1", playing[0].id);

    EXPECT_EQ(2u, m_catalog.listAllMovies().size());
}

TEST_F(CatalogTest, ListsTheatersShowingAMovie)
{
    const std::vector<Theater> theaters = m_catalog.listTheatersShowingMovie("m1");

    ASSERT_EQ(2u, theaters.size());
    EXPECT_EQ("t1", theaters[0].id);
    EXPECT_EQ("t2", theaters[1].id);

    EXPECT_TRUE(m_catalog.listTheatersShowingMovie("m2").empty());
}

TEST_F(CatalogTest, AsksAboutAnUnknownMovieAreNotFoundRatherThanEmpty)
{
    // A client must be able to tell "no such film" from "not on anywhere".
    EXPECT_SERVICE_ERROR(m_catalog.listTheatersShowingMovie("nope"),
                         ErrorCode::NotFound);
}

TEST_F(CatalogTest, ShowtimesComeBackInTimeOrder)
{
    const std::vector<Showtime> showtimes = m_catalog.listShowtimes("m1", "t1");

    ASSERT_EQ(2u, showtimes.size());
    EXPECT_EQ(9 * 60, showtimes[0].startTime);
    EXPECT_EQ(11 * 60, showtimes[1].startTime);
}

TEST_F(CatalogTest, SeatsFollowTheTheaterLayout)
{
    const std::vector<SeatLabel> seatsT1 = m_catalog.listAvailableSeats("s1");
    EXPECT_EQ(20u, seatsT1.size());
    EXPECT_EQ("a1", seatsT1.front());
    EXPECT_EQ("b10", seatsT1.back());

    // t2 is 12 seats at 6 per row -> a1..a6, b1..b6.
    const std::vector<SeatLabel> seatsT2 = m_catalog.listAvailableSeats("s3");
    EXPECT_EQ(12u, seatsT2.size());
    EXPECT_EQ("b6", seatsT2.back());
}

/* ---------------------------------------------------------------------------
 * Booking
 * ------------------------------------------------------------------------- */

TEST_F(CatalogTest, BookingRemovesSeatsFromAvailability)
{
    const std::vector<SeatLabel> wanted = {"a1", "a2"};
    const Booking booking = m_catalog.bookSeats("s1", wanted, "Alice");

    EXPECT_FALSE(booking.id.empty());
    EXPECT_EQ("s1", booking.showtimeId);
    EXPECT_EQ("Alice", booking.customerName);
    EXPECT_EQ(2u, booking.seats.size());
    EXPECT_GT(booking.createdAtEpochSeconds, 0);

    const std::vector<SeatLabel> available = m_catalog.listAvailableSeats("s1");
    EXPECT_EQ(18u, available.size());
    EXPECT_FALSE(contains(available, "a1"));
    EXPECT_FALSE(contains(available, "a2"));
}

TEST_F(CatalogTest, BookingDoesNotAffectOtherShowtimes)
{
    // Seat a1 at 09:00 and seat a1 at 11:00 are different seats.
    m_catalog.bookSeats("s1", {"a1"}, "Alice");

    EXPECT_EQ(20u, m_catalog.listAvailableSeats("s2").size());
}

TEST_F(CatalogTest, BookingATakenSeatIsRejected)
{
    m_catalog.bookSeats("s1", {"a1"}, "Alice");

    EXPECT_SERVICE_ERROR(m_catalog.bookSeats("s1", {"a1"}, "Bob"),
                         ErrorCode::SeatUnavailable);
}

TEST_F(CatalogTest, PartiallyUnavailableRequestBooksNothingAtAll)
{
    // The all-or-nothing rule. a3 must remain free after the failure.
    m_catalog.bookSeats("s1", {"a1"}, "Alice");

    EXPECT_SERVICE_ERROR(m_catalog.bookSeats("s1", {"a3", "a1"}, "Bob"),
                         ErrorCode::SeatUnavailable);

    const std::vector<SeatLabel> available = m_catalog.listAvailableSeats("s1");
    EXPECT_EQ(19u, available.size());
    EXPECT_TRUE(contains(available, "a3"));

    EXPECT_TRUE(m_catalog.listBookings().size() == 1u);
}

TEST_F(CatalogTest, RejectsEmptyDuplicateAndUnknownSeatRequests)
{
    EXPECT_SERVICE_ERROR(m_catalog.bookSeats("s1", {}, "Alice"),
                         ErrorCode::InvalidArgument);

    EXPECT_SERVICE_ERROR(m_catalog.bookSeats("s1", {"a1", "a1"}, "Alice"),
                         ErrorCode::InvalidArgument);

    // "a1" and "A1" name the same seat, so this is also a duplicate.
    EXPECT_SERVICE_ERROR(m_catalog.bookSeats("s1", {"a1", "A1"}, "Alice"),
                         ErrorCode::InvalidArgument);

    EXPECT_SERVICE_ERROR(m_catalog.bookSeats("s1", {"z9"}, "Alice"),
                         ErrorCode::NotFound);

    EXPECT_SERVICE_ERROR(m_catalog.bookSeats("nope", {"a1"}, "Alice"),
                         ErrorCode::NotFound);

    // None of the above may have left a booking behind.
    EXPECT_TRUE(m_catalog.listBookings().empty());
    EXPECT_EQ(20u, m_catalog.listAvailableSeats("s1").size());
}

TEST_F(CatalogTest, CancellingReleasesTheSeats)
{
    const Booking booking = m_catalog.bookSeats("s1", {"a1", "a2"}, "Alice");

    m_catalog.cancelBooking(booking.id);

    EXPECT_EQ(20u, m_catalog.listAvailableSeats("s1").size());
    EXPECT_SERVICE_ERROR(m_catalog.getBooking(booking.id), ErrorCode::NotFound);

    // And the seats can be booked again.
    EXPECT_NO_THROW(m_catalog.bookSeats("s1", {"a1"}, "Bob"));
}

TEST_F(CatalogTest, CancellingAnUnknownBookingIsNotFound)
{
    EXPECT_SERVICE_ERROR(m_catalog.cancelBooking("b999"), ErrorCode::NotFound);
}

/* ---------------------------------------------------------------------------
 * Administration
 * ------------------------------------------------------------------------- */

TEST_F(CatalogTest, GeneratedIdsDoNotCollideWithSuppliedOnes)
{
    // The fixture supplied m1 and m2 explicitly.
    const Movie added = m_catalog.addMovie(makeMovie("", "Generated", 100));
    EXPECT_EQ("m3", added.id);
}

TEST_F(CatalogTest, DuplicateIdsAreRejected)
{
    EXPECT_SERVICE_ERROR(m_catalog.addMovie(makeMovie("m1", "Clash", 100)),
                         ErrorCode::Conflict);
    EXPECT_SERVICE_ERROR(m_catalog.addTheater(makeTheater("t1", "Clash", 20, 10)),
                         ErrorCode::Conflict);
}

TEST_F(CatalogTest, InvalidMoviesAreRejected)
{
    EXPECT_SERVICE_ERROR(m_catalog.addMovie(makeMovie("", "", 100)),
                         ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(m_catalog.addMovie(makeMovie("", "No duration", 0)),
                         ErrorCode::InvalidArgument);
}

TEST_F(CatalogTest, NewTheatersFallBackToTheConfiguredDefaultCapacity)
{
    Theater theater;
    theater.name = "Defaults Please";
    // capacity and row width left at 0 = "use the configured default"

    const Theater stored = m_catalog.addTheater(theater);

    EXPECT_EQ(20, stored.seatCapacity);
    EXPECT_EQ(10, stored.seatsPerRow);
}

TEST_F(CatalogTest, RemovingAMovieRemovesItsShowtimesAndBookings)
{
    m_catalog.bookSeats("s1", {"a1"}, "Alice");
    ASSERT_EQ(1u, m_catalog.listBookings().size());

    m_catalog.removeMovie("m1");

    EXPECT_SERVICE_ERROR(m_catalog.getShowtime("s1"), ErrorCode::NotFound);
    EXPECT_SERVICE_ERROR(m_catalog.getShowtime("s3"), ErrorCode::NotFound);
    EXPECT_TRUE(m_catalog.listBookings().empty());
}

TEST_F(CatalogTest, RemovingATheaterRemovesOnlyItsOwnShowtimes)
{
    m_catalog.removeTheater("t1");

    EXPECT_SERVICE_ERROR(m_catalog.getShowtime("s1"), ErrorCode::NotFound);
    EXPECT_SERVICE_ERROR(m_catalog.getShowtime("s2"), ErrorCode::NotFound);
    EXPECT_NO_THROW(m_catalog.getShowtime("s3"));   // belongs to t2
}

TEST_F(CatalogTest, TwoShowtimesCannotShareAHallAndAMinute)
{
    Showtime clash;
    clash.movieId = "m2";
    clash.theaterId = "t1";
    clash.startTime = 9 * 60;   // s1 is already there

    EXPECT_SERVICE_ERROR(m_catalog.addShowtime(clash), ErrorCode::Conflict);
}

TEST_F(CatalogTest, ShowtimeSeatLayoutComesFromTheTheaterNotTheRequest)
{
    Showtime showtime;
    showtime.movieId = "m2";
    showtime.theaterId = "t2";      // 12 seats at 6 per row
    showtime.startTime = 15 * 60;
    showtime.seatCapacity = 999;    // must be ignored
    showtime.seatsPerRow = 999;

    const Showtime stored = m_catalog.addShowtime(showtime);

    EXPECT_EQ(12, stored.seatCapacity);
    EXPECT_EQ(6, stored.seatsPerRow);
}

TEST_F(CatalogTest, MovingAShowtimeKeepsItsBookings)
{
    const Booking booking = m_catalog.bookSeats("s1", {"a1"}, "Alice");

    m_catalog.updateShowtimeStart("s1", 10 * 60);

    EXPECT_EQ(10 * 60, m_catalog.getShowtime("s1").startTime);
    EXPECT_EQ(1u, m_catalog.getBooking(booking.id).seats.size());
    EXPECT_EQ(19u, m_catalog.listAvailableSeats("s1").size());
}

TEST_F(CatalogTest, MovingAShowtimeOntoAnotherIsRejected)
{
    EXPECT_SERVICE_ERROR(m_catalog.updateShowtimeStart("s1", 11 * 60),
                         ErrorCode::Conflict);
}

TEST_F(CatalogTest, EnlargingATheaterEnlargesItsShowtimes)
{
    Theater changes = makeTheater("t1", "Cinema Central", 40, 10);
    m_catalog.updateTheater("t1", changes);

    EXPECT_EQ(40u, m_catalog.listAvailableSeats("s1").size());
    EXPECT_EQ(40, m_catalog.getShowtime("s1").seatCapacity);
}

TEST_F(CatalogTest, ShrinkingATheaterIsRefusedIfABookedSeatWouldVanish)
{
    m_catalog.bookSeats("s1", {"b10"}, "Alice");   // index 19

    const Theater changes = makeTheater("t1", "Cinema Central", 10, 10);
    EXPECT_SERVICE_ERROR(m_catalog.updateTheater("t1", changes), ErrorCode::Conflict);

    // Nothing may have changed - not the theater, not the showtimes.
    EXPECT_EQ(20, m_catalog.getTheater("t1").seatCapacity);
    EXPECT_EQ(20, m_catalog.getShowtime("s1").seatCapacity);
    EXPECT_EQ(19u, m_catalog.listAvailableSeats("s1").size());
    EXPECT_EQ(20u, m_catalog.listAvailableSeats("s2").size());
}

TEST_F(CatalogTest, DefaultScheduleReplacesTheTimeline)
{
    moviebackend::ScheduleConfig config;   // 09:00 .. 21:00

    const std::vector<Showtime> created =
        m_catalog.applyDefaultSchedule("m1", "t1", config);

    // A 110 minute film: 09:00, 11:00, 13:00, 15:00, 17:00, 19:00, 21:00.
    ASSERT_EQ(7u, created.size());
    EXPECT_EQ(9 * 60, created.front().startTime);
    EXPECT_EQ(21 * 60, created.back().startTime);

    // The old s1/s2 are gone, replaced by the generated ones.
    EXPECT_SERVICE_ERROR(m_catalog.getShowtime("s1"), ErrorCode::NotFound);
    EXPECT_EQ(7u, m_catalog.listShowtimes("m1", "t1").size());
}

TEST_F(CatalogTest, DefaultScheduleSkipsSlotsAnotherFilmAlreadyOccupies)
{
    // Put m2 in t1 at 13:00, then generate m1's default timeline there.
    Showtime other;
    other.movieId = "m2";
    other.theaterId = "t1";
    other.startTime = 13 * 60;
    m_catalog.addShowtime(other);

    moviebackend::ScheduleConfig config;
    const std::vector<Showtime> created =
        m_catalog.applyDefaultSchedule("m1", "t1", config);

    // Six of the seven slots; 13:00 was taken.
    EXPECT_EQ(6u, created.size());
    for (std::size_t i = 0; i < created.size(); ++i) {
        EXPECT_NE(13 * 60, created[i].startTime);
    }
}

/* ---------------------------------------------------------------------------
 * Snapshot / restore
 * ------------------------------------------------------------------------- */

TEST_F(CatalogTest, SnapshotCapturesEverythingAndRestoreBringsItBack)
{
    const Booking booking = m_catalog.bookSeats("s1", {"a1", "a2"}, "Alice");
    const CatalogSnapshot snapshot = m_catalog.snapshot();

    EXPECT_EQ(2u, snapshot.movies.size());
    EXPECT_EQ(2u, snapshot.theaters.size());
    EXPECT_EQ(3u, snapshot.showtimes.size());
    EXPECT_EQ(1u, snapshot.bookings.size());

    // Restore into a completely separate catalog.
    Catalog other(m_defaults);
    other.restore(snapshot);

    EXPECT_EQ(18u, other.listAvailableSeats("s1").size());
    EXPECT_EQ("Alice", other.getBooking(booking.id).customerName);
    EXPECT_EQ(9 * 60, other.getShowtime("s1").startTime);
}

TEST_F(CatalogTest, RestoreRejectsInconsistentDataAndLeavesTheCatalogIntact)
{
    CatalogSnapshot broken;
    ShowtimeSnapshot orphan;
    orphan.showtime.id = "s9";
    orphan.showtime.movieId = "does-not-exist";
    orphan.showtime.theaterId = "also-not";
    orphan.showtime.startTime = 9 * 60;
    broken.showtimes.push_back(orphan);

    EXPECT_SERVICE_ERROR(m_catalog.restore(broken), ErrorCode::InvalidArgument);

    // The original world must be untouched.
    EXPECT_EQ(2u, m_catalog.listAllMovies().size());
    EXPECT_NO_THROW(m_catalog.getShowtime("s1"));
}

TEST_F(CatalogTest, RestoreRejectsTwoBookingsClaimingTheSameSeat)
{
    // Regression guard for the worst kind of bug this service can have:
    // a hand-edited catalog.json (which the admin reload endpoint invites)
    // used to load two bookings owning seat "a1". Cancelling one then freed a
    // seat the other still held - an over-booking created by the loader,
    // bypassing every lock in bookSeats().
    CatalogSnapshot snapshot;
    snapshot.movies.push_back(makeMovie("m1", "Film", 100));
    snapshot.theaters.push_back(makeTheater("t1", "Hall", 20, 10));

    ShowtimeSnapshot showtime;
    showtime.showtime.id = "s1";
    showtime.showtime.movieId = "m1";
    showtime.showtime.theaterId = "t1";
    showtime.showtime.startTime = 9 * 60;
    showtime.showtime.seatCapacity = 20;
    showtime.showtime.seatsPerRow = 10;
    showtime.bookedSeats.push_back("a1");
    snapshot.showtimes.push_back(showtime);

    Booking first;
    first.id = "b1";
    first.showtimeId = "s1";
    first.seats.push_back("a1");
    snapshot.bookings.push_back(first);

    Booking second;
    second.id = "b2";
    second.showtimeId = "s1";
    second.seats.push_back("a1");
    snapshot.bookings.push_back(second);

    EXPECT_SERVICE_ERROR(m_catalog.restore(snapshot), ErrorCode::InvalidArgument);
}

TEST_F(CatalogTest, RestoreRejectsADuplicateSeatSpeltDifferently)
{
    // "a1" and "A1" are the same seat, so this is the same bug wearing a hat.
    CatalogSnapshot snapshot;
    snapshot.movies.push_back(makeMovie("m1", "Film", 100));
    snapshot.theaters.push_back(makeTheater("t1", "Hall", 20, 10));

    ShowtimeSnapshot showtime;
    showtime.showtime.id = "s1";
    showtime.showtime.movieId = "m1";
    showtime.showtime.theaterId = "t1";
    showtime.showtime.startTime = 9 * 60;
    showtime.showtime.seatCapacity = 20;
    showtime.showtime.seatsPerRow = 10;
    showtime.bookedSeats.push_back("a1");
    snapshot.showtimes.push_back(showtime);

    Booking first;
    first.id = "b1";
    first.showtimeId = "s1";
    first.seats.push_back("a1");
    snapshot.bookings.push_back(first);

    Booking second;
    second.id = "b2";
    second.showtimeId = "s1";
    second.seats.push_back("A1");
    snapshot.bookings.push_back(second);

    EXPECT_SERVICE_ERROR(m_catalog.restore(snapshot), ErrorCode::InvalidArgument);
}

TEST_F(CatalogTest, RestoreRejectsABookingForASeatTheShowtimeDoesNotMarkBooked)
{
    // The seat map and the booking ledger are two views of one fact; loading a
    // file where they disagree would mean the seat is bookable *and* owned.
    CatalogSnapshot snapshot;
    snapshot.movies.push_back(makeMovie("m1", "Film", 100));
    snapshot.theaters.push_back(makeTheater("t1", "Hall", 20, 10));

    ShowtimeSnapshot showtime;
    showtime.showtime.id = "s1";
    showtime.showtime.movieId = "m1";
    showtime.showtime.theaterId = "t1";
    showtime.showtime.startTime = 9 * 60;
    showtime.showtime.seatCapacity = 20;
    showtime.showtime.seatsPerRow = 10;
    // deliberately no booked_seats
    snapshot.showtimes.push_back(showtime);

    Booking booking;
    booking.id = "b1";
    booking.showtimeId = "s1";
    booking.seats.push_back("a1");
    snapshot.bookings.push_back(booking);

    EXPECT_SERVICE_ERROR(m_catalog.restore(snapshot), ErrorCode::InvalidArgument);
}

TEST_F(CatalogTest, RestoreRejectsABookingForASeatOutsideTheLayout)
{
    CatalogSnapshot snapshot;
    snapshot.movies.push_back(makeMovie("m1", "Film", 100));
    snapshot.theaters.push_back(makeTheater("t1", "Hall", 20, 10));

    ShowtimeSnapshot showtime;
    showtime.showtime.id = "s1";
    showtime.showtime.movieId = "m1";
    showtime.showtime.theaterId = "t1";
    showtime.showtime.startTime = 9 * 60;
    showtime.showtime.seatCapacity = 20;
    showtime.showtime.seatsPerRow = 10;
    snapshot.showtimes.push_back(showtime);

    Booking booking;
    booking.id = "b1";
    booking.showtimeId = "s1";
    booking.seats.push_back("z99");
    snapshot.bookings.push_back(booking);

    EXPECT_SERVICE_ERROR(m_catalog.restore(snapshot), ErrorCode::InvalidArgument);
}

TEST_F(CatalogTest, AShowtimeMayMarkSeatsBookedWithNoBookingRecord)
{
    // The other direction is legitimate and the seed catalog relies on it:
    // a pre-sold house is expressed as booked_seats with no booking ledger.
    CatalogSnapshot snapshot;
    snapshot.movies.push_back(makeMovie("m1", "Film", 100));
    snapshot.theaters.push_back(makeTheater("t1", "Hall", 20, 10));

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

    ASSERT_NO_THROW(m_catalog.restore(snapshot));
    EXPECT_EQ(18u, m_catalog.listAvailableSeats("s1").size());
}

TEST(CatalogConstructionTest, RejectsConfiguredDefaultsThatCannotBeLabelled)
{
    // 1000 seats at 10 per row is 100 rows, and we only label a..z. Caught at
    // construction with a message naming the configuration, rather than much
    // later with one blaming catalog.json.
    TheaterDefaults tooManyRows;
    tooManyRows.seatCapacity = 1000;
    tooManyRows.seatsPerRow = 10;
    EXPECT_SERVICE_ERROR(Catalog{tooManyRows}, ErrorCode::InvalidArgument);

    TheaterDefaults negative;
    negative.seatCapacity = -1;
    negative.seatsPerRow = 10;
    EXPECT_SERVICE_ERROR(Catalog{negative}, ErrorCode::InvalidArgument);

    TheaterDefaults fine;   // 20 seats, 10 per row
    EXPECT_NO_THROW(Catalog{fine});
}

TEST_F(CatalogTest, RestoreReseedsIdGenerationSoNewIdsDoNotCollide)
{
    CatalogSnapshot snapshot;
    snapshot.movies.push_back(makeMovie("m77", "Loaded", 100));

    m_catalog.restore(snapshot);

    const Movie added = m_catalog.addMovie(makeMovie("", "Fresh", 100));
    EXPECT_EQ("m78", added.id);
}

} // unnamed namespace
