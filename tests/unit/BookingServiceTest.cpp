/**
 * @file BookingServiceTest.cpp
 * @brief Tests for the end-user facade.
 *
 * These walk the six-step user journey from the requirements in order, which
 * doubles as executable documentation of the intended client flow.
 */
#include "moviebackend/BookingService.hpp"

#include "moviebackend/Errors.hpp"
#include "TestSupport.hpp"

#include <gtest/gtest.h>

using moviebackend::Booking;
using moviebackend::BookingService;
using moviebackend::Catalog;
using moviebackend::ErrorCode;
using moviebackend::Movie;
using moviebackend::SeatLabel;
using moviebackend::ShowtimeAvailability;
using moviebackend::Theater;
using moviebackend::TheaterDefaults;
using moviebackend::testsupport::contains;
using moviebackend::testsupport::makeMovie;
using moviebackend::testsupport::makeTheater;

namespace {

class BookingServiceTest : public ::testing::Test {
protected:
    BookingServiceTest()
        : m_defaults()
        , m_catalog(m_defaults)
        , m_service(m_catalog)
    {
    }

    void SetUp() override
    {
        m_catalog.addMovie(makeMovie("m1", "The Silent Protocol", 110));
        m_catalog.addMovie(makeMovie("m2", "Not Scheduled", 90));

        m_catalog.addTheater(makeTheater("t1", "Cinema Central", 20, 10));
        m_catalog.addTheater(makeTheater("t2", "Riverside Plex", 12, 6));

        addShowtime("s1", "m1", "t1", 9 * 60);
        addShowtime("s2", "m1", "t1", 11 * 60);
        addShowtime("s3", "m1", "t2", 13 * 60);
    }

    void addShowtime(const std::string& id,
                     const std::string& movieId,
                     const std::string& theaterId,
                     int startTime)
    {
        moviebackend::Showtime showtime;
        showtime.id = id;
        showtime.movieId = movieId;
        showtime.theaterId = theaterId;
        showtime.startTime = startTime;
        m_catalog.addShowtime(showtime);
    }

    TheaterDefaults m_defaults;
    Catalog         m_catalog;
    BookingService  m_service;
};

TEST_F(BookingServiceTest, TheFullUserJourney)
{
    // Step 1 - view all playing movies.
    const std::vector<Movie> movies = m_service.listPlayingMovies();
    ASSERT_EQ(1u, movies.size());
    EXPECT_EQ("m1", movies[0].id);

    // Step 2 - select a movie.
    const Movie movie = m_service.getMovie(movies[0].id);
    EXPECT_EQ("The Silent Protocol", movie.title);

    // Step 3 - see all theaters showing it.
    const std::vector<Theater> theaters =
        m_service.listTheatersShowingMovie(movie.id);
    ASSERT_EQ(2u, theaters.size());

    // Step 4 - select a theater, see its timeline.
    const std::vector<ShowtimeAvailability> showtimes =
        m_service.listShowtimes(movie.id, theaters[0].id);
    ASSERT_EQ(2u, showtimes.size());
    EXPECT_EQ(20, showtimes[0].totalSeats);
    EXPECT_EQ(20, showtimes[0].availableSeats);

    // Step 5 - see available seats.
    const std::vector<SeatLabel> seats =
        m_service.listAvailableSeats(showtimes[0].showtime.id);
    ASSERT_EQ(20u, seats.size());
    EXPECT_EQ("a1", seats[0]);
    EXPECT_EQ("a2", seats[1]);
    EXPECT_EQ("a3", seats[2]);

    // Step 6 - book some of them.
    const Booking booking = m_service.bookSeats(
        showtimes[0].showtime.id, {"a1", "a2", "a3"}, "Alice");

    EXPECT_EQ(3u, booking.seats.size());
    EXPECT_EQ("Alice", booking.customerName);

    // And the availability reflects it.
    const std::vector<SeatLabel> remaining =
        m_service.listAvailableSeats(showtimes[0].showtime.id);
    EXPECT_EQ(17u, remaining.size());
    EXPECT_FALSE(contains(remaining, "a1"));
}

TEST_F(BookingServiceTest, AvailabilityCountsShrinkAsSeatsAreBooked)
{
    m_service.bookSeats("s1", {"a1", "a2"}, "Alice");

    const std::vector<ShowtimeAvailability> showtimes =
        m_service.listShowtimes("m1", "t1");

    ASSERT_EQ(2u, showtimes.size());

    // s1 is the 09:00 one and comes first.
    EXPECT_EQ("s1", showtimes[0].showtime.id);
    EXPECT_EQ(18, showtimes[0].availableSeats);
    EXPECT_EQ(20, showtimes[0].totalSeats);

    // s2 is untouched.
    EXPECT_EQ(20, showtimes[1].availableSeats);
}

TEST_F(BookingServiceTest, AvailabilityReflectsTheTheatersOwnCapacity)
{
    const std::vector<ShowtimeAvailability> showtimes =
        m_service.listShowtimes("m1", "t2");

    ASSERT_EQ(1u, showtimes.size());
    EXPECT_EQ(12, showtimes[0].totalSeats);
    EXPECT_EQ(12, showtimes[0].availableSeats);
}

TEST_F(BookingServiceTest, UnknownEntitiesAreNotFound)
{
    EXPECT_SERVICE_ERROR(m_service.getMovie("nope"), ErrorCode::NotFound);
    EXPECT_SERVICE_ERROR(m_service.listTheatersShowingMovie("nope"),
                         ErrorCode::NotFound);
    EXPECT_SERVICE_ERROR(m_service.listShowtimes("m1", "nope"), ErrorCode::NotFound);
    EXPECT_SERVICE_ERROR(m_service.listAvailableSeats("nope"), ErrorCode::NotFound);
    EXPECT_SERVICE_ERROR(m_service.bookSeats("nope", {"a1"}, "Alice"),
                         ErrorCode::NotFound);
    EXPECT_SERVICE_ERROR(m_service.getBooking("nope"), ErrorCode::NotFound);
}

TEST_F(BookingServiceTest, DoubleBookingIsRejected)
{
    m_service.bookSeats("s1", {"a1"}, "Alice");

    EXPECT_SERVICE_ERROR(m_service.bookSeats("s1", {"a1"}, "Bob"),
                         ErrorCode::SeatUnavailable);
}

TEST_F(BookingServiceTest, BookingReceiptCanBeRetrievedAndCancelled)
{
    const Booking booking = m_service.bookSeats("s1", {"a5"}, "Alice");

    const Booking fetched = m_service.getBooking(booking.id);
    EXPECT_EQ(booking.id, fetched.id);
    EXPECT_EQ("a5", fetched.seats.front());

    m_service.cancelBooking(booking.id);

    EXPECT_SERVICE_ERROR(m_service.getBooking(booking.id), ErrorCode::NotFound);
    EXPECT_TRUE(contains(m_service.listAvailableSeats("s1"), "a5"));
}

TEST_F(BookingServiceTest, AMovieWithNoShowtimesIsNotOfferedToUsers)
{
    // m2 exists but is not scheduled, so an end user never sees it in the
    // listing - though asking for it directly still works.
    const std::vector<Movie> playing = m_service.listPlayingMovies();

    for (std::size_t i = 0; i < playing.size(); ++i) {
        EXPECT_NE("m2", playing[i].id);
    }

    EXPECT_NO_THROW(m_service.getMovie("m2"));
    EXPECT_TRUE(m_service.listTheatersShowingMovie("m2").empty());
}

} // unnamed namespace
