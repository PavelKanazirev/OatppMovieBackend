/**
 * @file AdminService.cpp
 * @brief Administrative use cases.
 */
#include "moviebackend/AdminService.hpp"

#include "moviebackend/Errors.hpp"
#include "moviebackend/Logging.hpp"
#include "moviebackend/Schedule.hpp"

namespace moviebackend {

AdminService::AdminService(Catalog& catalog,
                           CatalogStore& store,
                           const ScheduleConfig& schedule)
    : m_catalog(catalog)
    , m_store(store)
    , m_schedule(schedule)
{
}

/* =========================================================================
 * Theaters
 * ========================================================================= */

std::vector<Theater> AdminService::listTheaters() const
{
    return m_catalog.listTheaters();
}

Theater AdminService::addTheater(const Theater& theater)
{
    MB_LOG_INFO("admin: adding theater '{}'", theater.name);
    return m_catalog.addTheater(theater);
}

Theater AdminService::updateTheater(const TheaterId& theaterId, const Theater& changes)
{
    MB_LOG_INFO("admin: updating theater {}", theaterId);
    return m_catalog.updateTheater(theaterId, changes);
}

void AdminService::removeTheater(const TheaterId& theaterId)
{
    MB_LOG_INFO("admin: removing theater {}", theaterId);
    m_catalog.removeTheater(theaterId);
}

/* =========================================================================
 * Movies
 * ========================================================================= */

std::vector<Movie> AdminService::listMovies() const
{
    // The admin view lists *all* movies, including ones with no showtimes -
    // the administrator needs to see a film in order to schedule it. This is
    // the one deliberate difference from BookingService::listPlayingMovies().
    return m_catalog.listAllMovies();
}

Movie AdminService::addMovie(const Movie& movie)
{
    MB_LOG_INFO("admin: adding movie '{}'", movie.title);
    return m_catalog.addMovie(movie);
}

Movie AdminService::updateMovie(const MovieId& movieId, const Movie& changes)
{
    MB_LOG_INFO("admin: updating movie {}", movieId);
    return m_catalog.updateMovie(movieId, changes);
}

void AdminService::removeMovie(const MovieId& movieId)
{
    MB_LOG_INFO("admin: removing movie {}", movieId);
    m_catalog.removeMovie(movieId);
}

/* =========================================================================
 * Timelines
 * ========================================================================= */

std::vector<Showtime> AdminService::listShowtimes(const MovieId& movieId,
                                                  const TheaterId& theaterId) const
{
    return m_catalog.listShowtimes(movieId, theaterId);
}

std::vector<Showtime> AdminService::listAllShowtimes() const
{
    return m_catalog.listAllShowtimes();
}

std::vector<Showtime> AdminService::applyDefaultSchedule(const MovieId& movieId,
                                                         const TheaterId& theaterId)
{
    MB_LOG_INFO("admin: applying default schedule for movie {} in theater {}",
                movieId, theaterId);
    return m_catalog.applyDefaultSchedule(movieId, theaterId, m_schedule);
}

Showtime AdminService::addShowtime(const MovieId& movieId,
                                   const TheaterId& theaterId,
                                   MinutesSinceMidnight startTime)
{
    // The configured day window is a *service-level* policy, not a catalog
    // invariant - the catalog will happily store a 03:00 screening if a
    // future feature wants one. Enforcing it here keeps that policy in one
    // place and out of the locked code.
    if (startTime < m_schedule.dayStart || startTime > m_schedule.dayEnd) {
        throw invalidArgument(
            "start time " + schedule::formatTimeOfDay(startTime)
            + " is outside the configured screening day ("
            + schedule::formatTimeOfDay(m_schedule.dayStart) + ".."
            + schedule::formatTimeOfDay(m_schedule.dayEnd) + ")");
    }

    Showtime showtime;
    showtime.movieId = movieId;
    showtime.theaterId = theaterId;
    showtime.startTime = startTime;

    MB_LOG_INFO("admin: adding showtime for movie {} in theater {} at {}",
                movieId, theaterId, schedule::formatTimeOfDay(startTime));

    return m_catalog.addShowtime(showtime);
}

Showtime AdminService::updateShowtime(const ShowtimeId& showtimeId,
                                      MinutesSinceMidnight newStartTime)
{
    if (newStartTime < m_schedule.dayStart || newStartTime > m_schedule.dayEnd) {
        throw invalidArgument(
            "start time " + schedule::formatTimeOfDay(newStartTime)
            + " is outside the configured screening day ("
            + schedule::formatTimeOfDay(m_schedule.dayStart) + ".."
            + schedule::formatTimeOfDay(m_schedule.dayEnd) + ")");
    }

    MB_LOG_INFO("admin: moving showtime {} to {}",
                showtimeId, schedule::formatTimeOfDay(newStartTime));

    return m_catalog.updateShowtimeStart(showtimeId, newStartTime);
}

void AdminService::removeShowtime(const ShowtimeId& showtimeId)
{
    MB_LOG_INFO("admin: removing showtime {}", showtimeId);
    m_catalog.removeShowtime(showtimeId);
}

/* =========================================================================
 * Persistence control
 * ========================================================================= */

void AdminService::saveCatalog()
{
    MB_LOG_INFO("admin: forcing a synchronous catalog save");

    // Synchronous on the caller's thread, unlike the autosave path. The
    // administrator asked for a checkpoint and gets an HTTP response only
    // once the bytes are on disk.
    const CatalogSnapshot snapshot = m_catalog.snapshot();
    m_store.save(snapshot);
}

void AdminService::reloadCatalog()
{
    MB_LOG_WARN("admin: reloading the catalog from disk - in-memory changes "
                "since the last save will be lost");

    const CatalogSnapshot snapshot = m_store.load();
    m_catalog.restore(snapshot);
}

} // namespace moviebackend
