/**
 * @file BookingService.cpp
 * @brief End-user use cases. A thin facade over Catalog.
 *
 * Most methods here forward straight to the catalog. That is intentional -
 * the value of the class is that it *names* the six steps of the user journey
 * and gives the REST layer one type to depend on, not that it adds logic.
 * The one method that does more than forward is listShowtimes(), which joins
 * a showtime with its seat counts.
 */
#include "moviebackend/BookingService.hpp"

#include "moviebackend/Errors.hpp"
#include "moviebackend/Logging.hpp"

namespace moviebackend {

BookingService::BookingService(Catalog& catalog)
    : m_catalog(catalog)
{
}

std::vector<Movie> BookingService::listPlayingMovies() const
{
    MB_LOG_DEBUG("listing playing movies");
    return m_catalog.listPlayingMovies();
}

Movie BookingService::getMovie(const MovieId& movieId) const
{
    MB_LOG_DEBUG("fetching movie {}", movieId);
    return m_catalog.getMovie(movieId);
}

std::vector<Theater> BookingService::listTheatersShowingMovie(
    const MovieId& movieId) const
{
    MB_LOG_DEBUG("listing theaters showing movie {}", movieId);
    return m_catalog.listTheatersShowingMovie(movieId);
}

std::vector<ShowtimeAvailability> BookingService::listShowtimes(
    const MovieId& movieId,
    const TheaterId& theaterId) const
{
    MB_LOG_DEBUG("listing showtimes for movie {} in theater {}", movieId, theaterId);

    const std::vector<Showtime> showtimes =
        m_catalog.listShowtimes(movieId, theaterId);

    std::vector<ShowtimeAvailability> result;
    result.reserve(showtimes.size());

    for (std::size_t i = 0; i < showtimes.size(); ++i) {
        ShowtimeAvailability entry;
        entry.showtime = showtimes[i];
        entry.totalSeats = showtimes[i].seatCapacity;

        // One catalog call per showtime, each taking the lock briefly, rather
        // than one call holding the lock across the whole loop. The counts
        // are advisory anyway - a seat can be taken a microsecond after the
        // response is sent - so a perfectly consistent view across all
        // showtimes would buy nothing and would lengthen the critical section
        // that bookings compete for.
        const std::vector<SeatLabel> available =
            m_catalog.listAvailableSeats(showtimes[i].id);
        entry.availableSeats = static_cast<int>(available.size());

        result.push_back(entry);
    }

    return result;
}

Showtime BookingService::getShowtime(const ShowtimeId& showtimeId) const
{
    MB_LOG_DEBUG("fetching showtime {}", showtimeId);
    return m_catalog.getShowtime(showtimeId);
}

std::vector<SeatLabel> BookingService::listAvailableSeats(
    const ShowtimeId& showtimeId) const
{
    MB_LOG_DEBUG("listing available seats for showtime {}", showtimeId);
    return m_catalog.listAvailableSeats(showtimeId);
}

std::vector<SeatLabel> BookingService::listBookedSeats(
    const ShowtimeId& showtimeId) const
{
    MB_LOG_DEBUG("listing booked seats for showtime {}", showtimeId);
    return m_catalog.listBookedSeats(showtimeId);
}

Booking BookingService::bookSeats(const ShowtimeId& showtimeId,
                                  const std::vector<SeatLabel>& seats,
                                  const std::string& customerName)
{
    MB_LOG_DEBUG("booking {} seat(s) for showtime {}", seats.size(), showtimeId);

    // Straight delegation on purpose. Every check that matters for
    // over-booking has to happen inside the catalog's critical section; doing
    // any of it here, outside the lock, would be a check-then-act race even
    // though the code would look reasonable.
    return m_catalog.bookSeats(showtimeId, seats, customerName);
}

Booking BookingService::getBooking(const BookingId& bookingId) const
{
    MB_LOG_DEBUG("fetching booking {}", bookingId);
    return m_catalog.getBooking(bookingId);
}

void BookingService::cancelBooking(const BookingId& bookingId)
{
    MB_LOG_DEBUG("cancelling booking {}", bookingId);
    m_catalog.cancelBooking(bookingId);
}

} // namespace moviebackend
