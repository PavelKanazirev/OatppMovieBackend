/**
 * @file BookingService.hpp
 * @brief The end-user facing use cases: browse, then book.
 *
 * This is the **public API of the backend for a normal client**. The REST
 * controllers are a thin translation layer on top of it - anybody embedding
 * this backend without HTTP would program against this header.
 *
 * Design pattern: **Facade**. Catalog exposes fine-grained operations; this
 * class exposes the handful of coarse use cases the requirements list, in the
 * order a user performs them:
 *
 *     view all playing movies
 *       -> select a movie
 *         -> see theaters showing it
 *           -> select a theater
 *             -> see the showtimes
 *               -> see available seats
 *                 -> book seats
 *
 * Thread safety: this class holds no state of its own beyond a reference to
 * the catalog, so it inherits the catalog's thread safety. One instance is
 * shared by every request thread.
 */
#ifndef MOVIEBACKEND_BOOKINGSERVICE_HPP
#define MOVIEBACKEND_BOOKINGSERVICE_HPP

#include "moviebackend/Catalog.hpp"
#include "moviebackend/Domain.hpp"

#include <string>
#include <vector>

namespace moviebackend {

/** @brief A showtime paired with the derived seat-availability counts. */
struct ShowtimeAvailability {
    Showtime showtime;

    /** Seats free right now. A snapshot - it can change immediately after. */
    int availableSeats = 0;

    /** Total seats for this screening. */
    int totalSeats = 0;
};

/**
 * @brief Read-and-book operations for end users.
 */
class BookingService {
public:
    /**
     * @param catalog the shared catalog; must outlive this service
     */
    explicit BookingService(Catalog& catalog);

    BookingService(const BookingService&) = delete;
    BookingService& operator=(const BookingService&) = delete;

    /**
     * @brief Step 1 - every movie currently on somewhere.
     *
     * "Playing" means the movie has at least one showtime. A movie that
     * exists in the catalog but is not scheduled anywhere is not listed here;
     * the admin API lists those.
     */
    std::vector<Movie> listPlayingMovies() const;

    /**
     * @brief Step 2 - details of the movie the user selected.
     * @throws ServiceError NotFound.
     */
    Movie getMovie(const MovieId& movieId) const;

    /**
     * @brief Step 3 - theaters showing the selected movie.
     * @throws ServiceError NotFound if the movie is unknown.
     */
    std::vector<Theater> listTheatersShowingMovie(const MovieId& movieId) const;

    /**
     * @brief Step 4 - screenings of that movie in the selected theater, with
     *        a seat count so the client can grey out sold-out slots.
     * @throws ServiceError NotFound if either entity is unknown.
     */
    std::vector<ShowtimeAvailability> listShowtimes(
        const MovieId& movieId,
        const TheaterId& theaterId) const;

    /**
     * @brief Look up one screening.
     *
     * Needed by a client that holds only a showtime id (from a bookmark or a
     * previous response) and wants its layout before asking for seats.
     *
     * @throws ServiceError NotFound if the showtime is unknown.
     */
    Showtime getShowtime(const ShowtimeId& showtimeId) const;

    /**
     * @brief Step 5 - free seats for a screening, e.g. {"a1","a2","a3"}.
     * @throws ServiceError NotFound if the showtime is unknown.
     */
    std::vector<SeatLabel> listAvailableSeats(const ShowtimeId& showtimeId) const;

    /**
     * @brief The seats already taken for a screening.
     *
     * Together with listAvailableSeats() this lets a seat-map UI render the
     * whole auditorium from a single response instead of inferring the taken
     * seats from the capacity.
     *
     * @throws ServiceError NotFound if the showtime is unknown.
     */
    std::vector<SeatLabel> listBookedSeats(const ShowtimeId& showtimeId) const;

    /**
     * @brief Step 6 - reserve seats.
     *
     * Delegates straight to Catalog::bookSeats, which performs the whole
     * check-then-act under one lock. All or nothing: a request for
     * {"a1","a2"} where a2 is taken books neither seat.
     *
     * @param showtimeId   the screening
     * @param seats        the seats to reserve, at least one
     * @param customerName free-form label for the booking
     * @return the confirmed booking
     * @throws ServiceError InvalidArgument, NotFound or SeatUnavailable -
     *         see Catalog::bookSeats.
     */
    Booking bookSeats(const ShowtimeId& showtimeId,
                      const std::vector<SeatLabel>& seats,
                      const std::string& customerName);

    /**
     * @brief Retrieve a booking receipt.
     * @throws ServiceError NotFound.
     */
    Booking getBooking(const BookingId& bookingId) const;

    /**
     * @brief Cancel a booking, releasing its seats back to the pool.
     * @throws ServiceError NotFound.
     */
    void cancelBooking(const BookingId& bookingId);

private:
    Catalog& m_catalog;
};

} // namespace moviebackend

#endif // MOVIEBACKEND_BOOKINGSERVICE_HPP
