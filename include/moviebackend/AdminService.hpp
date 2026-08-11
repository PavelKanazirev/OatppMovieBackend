/**
 * @file AdminService.hpp
 * @brief The privileged use cases: editing the catalog at run time.
 *
 * This is the **public API of the backend for the special/administrative
 * user**. It is a separate class from BookingService for the same reason the
 * REST routes are separate: the two audiences are different, and keeping the
 * mutating operations in their own type makes it obvious at a glance which
 * code paths can change the catalog.
 *
 * Design pattern: **Facade**, same as BookingService.
 *
 * Thread safety: stateless apart from references, therefore as thread safe as
 * the Catalog it delegates to.
 *
 * Note on authorisation: the exercise explicitly defers the question of *how*
 * the special user is distinguished from an end user - separate routes are
 * enough for now. There is no authentication anywhere in this service, and
 * that is a deliberate, documented gap rather than an oversight.
 */
#ifndef MOVIEBACKEND_ADMINSERVICE_HPP
#define MOVIEBACKEND_ADMINSERVICE_HPP

#include "moviebackend/Catalog.hpp"
#include "moviebackend/CatalogStore.hpp"
#include "moviebackend/Config.hpp"
#include "moviebackend/Domain.hpp"

#include <string>
#include <vector>

namespace moviebackend {

/**
 * @brief Catalog administration operations.
 */
class AdminService {
public:
    /**
     * @param catalog  the shared catalog; must outlive this service
     * @param store    the persistence backend, used by saveCatalog() and
     *                 reloadCatalog(); must outlive this service
     * @param schedule day bounds used when generating default timelines
     */
    AdminService(Catalog& catalog,
                 CatalogStore& store,
                 const ScheduleConfig& schedule);

    AdminService(const AdminService&) = delete;
    AdminService& operator=(const AdminService&) = delete;

    /* ---------------------------------------------------------------------
     * Theaters
     * ------------------------------------------------------------------- */

    /** @brief Every theater, sorted by id. */
    std::vector<Theater> listTheaters() const;

    /**
     * @brief Append a theater.
     * @param theater capacity/row width of 0 mean "use the configured default"
     * @throws ServiceError InvalidArgument / Conflict.
     */
    Theater addTheater(const Theater& theater);

    /**
     * @brief Modify a theater.
     *
     * Changing the seating layout re-lays out every showtime of this theater.
     * A shrink that would drop an already-booked seat is refused with
     * Conflict and nothing is changed.
     *
     * @throws ServiceError NotFound / InvalidArgument / Conflict.
     */
    Theater updateTheater(const TheaterId& theaterId, const Theater& changes);

    /**
     * @brief Remove a theater, its showtimes and their bookings.
     * @throws ServiceError NotFound.
     */
    void removeTheater(const TheaterId& theaterId);

    /* ---------------------------------------------------------------------
     * Movies
     * ------------------------------------------------------------------- */

    /** @brief Every movie, including ones not currently scheduled. */
    std::vector<Movie> listMovies() const;

    /**
     * @brief Append a movie.
     * @throws ServiceError InvalidArgument / Conflict.
     */
    Movie addMovie(const Movie& movie);

    /**
     * @brief Modify a movie.
     *
     * Changing the duration does **not** reshuffle existing showtimes -
     * that would move screenings customers have already booked. Call
     * applyDefaultSchedule() explicitly if that is what you want.
     *
     * @throws ServiceError NotFound / InvalidArgument.
     */
    Movie updateMovie(const MovieId& movieId, const Movie& changes);

    /**
     * @brief Remove a movie, its showtimes and their bookings.
     * @throws ServiceError NotFound.
     */
    void removeMovie(const MovieId& movieId);

    /* ---------------------------------------------------------------------
     * Timelines (showtimes)
     * ------------------------------------------------------------------- */

    /**
     * @brief The timeline of one movie in one theater, ascending.
     * @throws ServiceError NotFound.
     */
    std::vector<Showtime> listShowtimes(const MovieId& movieId,
                                        const TheaterId& theaterId) const;

    /** @brief Every showtime in the catalog. */
    std::vector<Showtime> listAllShowtimes() const;

    /**
     * @brief Give a movie its default timeline in a theater.
     *
     * Generates 09:00, then each following whole hour after the previous
     * screening ends, up to 21:00 - the rule spelled out in
     * schedule::generateDefaultStartTimes.
     *
     * **Destructive**: any existing showtimes for this (movie, theater) pair
     * and their bookings are removed first.
     *
     * @throws ServiceError NotFound if the movie or theater is unknown.
     */
    std::vector<Showtime> applyDefaultSchedule(const MovieId& movieId,
                                               const TheaterId& theaterId);

    /**
     * @brief Append a single time slot.
     * @param movieId   the movie
     * @param theaterId the theater
     * @param startTime start time, must lie inside the configured day
     * @throws ServiceError NotFound, InvalidArgument if the time is outside
     *         the configured day, Conflict if the theater is already busy
     *         at that minute.
     */
    Showtime addShowtime(const MovieId& movieId,
                         const TheaterId& theaterId,
                         MinutesSinceMidnight startTime);

    /**
     * @brief Move an existing time slot.
     * @throws ServiceError NotFound / InvalidArgument / Conflict.
     */
    Showtime updateShowtime(const ShowtimeId& showtimeId,
                            MinutesSinceMidnight newStartTime);

    /**
     * @brief Remove a time slot and every booking against it.
     * @throws ServiceError NotFound.
     */
    void removeShowtime(const ShowtimeId& showtimeId);

    /* ---------------------------------------------------------------------
     * Persistence control
     * ------------------------------------------------------------------- */

    /**
     * @brief Write the catalog to its JSON file right now, synchronously.
     *
     * Autosave normally handles this in the background; this endpoint exists
     * so an administrator can force a checkpoint and know it landed.
     *
     * @throws ServiceError PersistenceFailure.
     */
    void saveCatalog();

    /**
     * @brief Discard in-memory state and re-read the JSON file.
     *
     * Intended for the case where the administrator edited the file by hand.
     * **Destructive**: bookings made since the last save are lost.
     *
     * @throws ServiceError PersistenceFailure / InvalidArgument.
     */
    void reloadCatalog();

private:
    Catalog&       m_catalog;
    CatalogStore&  m_store;
    ScheduleConfig m_schedule;
};

} // namespace moviebackend

#endif // MOVIEBACKEND_ADMINSERVICE_HPP
