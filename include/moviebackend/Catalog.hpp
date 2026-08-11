/**
 * @file Catalog.hpp
 * @brief The single in-memory source of truth, and the only place in the
 *        service where a lock is held.
 *
 * =========================================================================
 *  THE CONCURRENCY DESIGN - read this before changing anything in here
 * =========================================================================
 *
 * The core correctness requirement of the exercise is: *many simultaneous
 * booking requests, never an over-booking*. oatpp serves each HTTP request on
 * its own thread, so every public method below can be entered concurrently.
 *
 * ## One coarse mutex, deliberately
 *
 * Catalog owns exactly one `std::mutex` that guards *all* of its state. Not a
 * mutex per showtime, not a reader/writer lock, no atomics, no lock-free
 * anything.
 *
 * Why that is the right call here rather than laziness:
 *
 *   * **It makes over-booking structurally impossible.** Booking a set of
 *     seats is a check-then-act sequence: verify every requested seat is
 *     free, then mark them all. With one lock held across the whole
 *     sequence, no other thread can observe or modify the seat map in
 *     between. This is *the* bug the exercise is about, and a single lock
 *     removes it by construction rather than by careful reasoning.
 *
 *   * **Multi-entity operations stay atomic.** Deleting a theater also
 *     deletes its showtimes and their bookings. With per-entity locks that
 *     becomes a lock-ordering problem with a genuine deadlock risk.
 *
 *   * **The critical sections are microseconds long.** They are pure
 *     in-memory map lookups and a bitmap write - no I/O, no allocation of
 *     any size, no callbacks into other subsystems. Contention on a lock
 *     this short is not the bottleneck; the network stack is.
 *
 * A `std::shared_mutex` would let the read-only endpoints run in parallel and
 * is the obvious next step if profiling ever justified it. It is not used
 * because it would buy nothing measurable here while adding a second way to
 * get the locking wrong.
 *
 * ## Two rules that keep it safe
 *
 * 1. **No public method calls another public method.** `std::mutex` is not
 *    recursive, so re-entering would self-deadlock. Every public method locks
 *    once and then does its work through the private `*Locked` helpers, which
 *    assume the lock is already held. The `Locked` suffix is the whole
 *    convention - if a function name ends in it, the caller owns the lock.
 *
 * 2. **Nothing is returned by reference.** Every getter returns a *copy*.
 *    Handing a caller a reference into the maps would let it read the data
 *    after the lock was released, which is a data race that compiles
 *    perfectly and fails once a month in production. Copying a handful of
 *    small structs is cheap; a race is not.
 *
 * ## The change callback
 *
 * Catalog notifies one observer after any mutation, so the persistence
 * worker knows the catalog is dirty. The notification is fired *after the
 * lock is released* - calling out to unknown code while holding a lock is
 * how lock inversions are born.
 */
#ifndef MOVIEBACKEND_CATALOG_HPP
#define MOVIEBACKEND_CATALOG_HPP

#include "moviebackend/Config.hpp"
#include "moviebackend/Domain.hpp"
#include "moviebackend/IdGenerator.hpp"
#include "moviebackend/SeatMap.hpp"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace moviebackend {

/**
 * @brief Observer notified whenever the catalog changes.
 *
 * Design pattern: **Observer**. Implemented as an abstract base class rather
 * than a `std::function` because the only implementation is a long-lived
 * object (PersistenceWorker) and an interface documents the contract - in
 * particular the `noexcept` requirement - far more visibly than a callable
 * type alias would.
 *
 * CALLBACK: onCatalogChanged() is the single "state was mutated" event in the
 * whole system. It is invoked from whichever request thread performed the
 * mutation, with **no Catalog lock held**, so the implementation may take its
 * own locks freely. It must not throw and must return quickly - do the real
 * work on your own thread.
 */
class CatalogChangeListener {
public:
    virtual ~CatalogChangeListener() = default;

    /** @brief Called after any mutation of the catalog. Must not throw. */
    virtual void onCatalogChanged() noexcept = 0;

protected:
    CatalogChangeListener() = default;

    // Not copyable: listeners are registered by address.
    CatalogChangeListener(const CatalogChangeListener&) = delete;
    CatalogChangeListener& operator=(const CatalogChangeListener&) = delete;
};

/**
 * @brief In-memory store of movies, theaters, showtimes and bookings.
 *
 * Thread safe: every public method may be called from any thread at any time.
 */
class Catalog {
public:
    /**
     * @param defaults seating layout applied when a theater does not carry
     *                 its own, straight from config.json
     */
    explicit Catalog(const TheaterDefaults& defaults);

    ~Catalog();

    // Non-copyable, non-movable: it owns a mutex and is referred to by
    // address from the REST controllers and the persistence worker.
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    /* ---------------------------------------------------------------------
     * Change notification
     * ------------------------------------------------------------------- */

    /**
     * @brief Register (or clear, with nullptr) the change observer.
     *
     * Ownership: **not** taken. The pointer is a non-owning observer
     * reference - deliberately a raw pointer and not a std::shared_ptr,
     * because the listener is guaranteed to outlive its registration by
     * construction order in main(): the Catalog is created first and
     * destroyed last, and PersistenceWorker unregisters itself in its own
     * destructor. Making this a shared_ptr would imply shared ownership that
     * does not exist.
     *
     * Detaching (passing nullptr) **blocks until every in-flight callback has
     * returned**, so that when this call completes no thread is executing
     * inside the previous listener. PersistenceWorker's destructor depends on
     * that: it detaches and then destroys the object the callback runs on.
     *
     * @param listener the observer, or nullptr to detach
     */
    void setChangeListener(CatalogChangeListener* listener);

    /* ---------------------------------------------------------------------
     * Bulk load / snapshot - used by the persistence layer
     * ------------------------------------------------------------------- */

    /**
     * @brief Replace the entire contents of the catalog.
     *
     * Atomic from an observer's point of view: either the whole snapshot is
     * in place or none of it is. Also re-seeds the id generators so that
     * freshly minted ids cannot collide with loaded ones.
     *
     * @param snapshot the state to install
     * @throws ServiceError InvalidArgument if the snapshot is internally
     *         inconsistent (a showtime referring to a missing movie, a booked
     *         seat outside the seat map, duplicate ids, ...). The catalog is
     *         left untouched if validation fails.
     */
    void restore(const CatalogSnapshot& snapshot);

    /**
     * @brief Take a consistent copy of everything.
     *
     * CRITICAL SECTION: holds m_mutex for the duration of the copy. This is
     * the longest critical section in the system, and it is still just a walk
     * over a few maps.
     */
    CatalogSnapshot snapshot() const;

    /* ---------------------------------------------------------------------
     * Queries (end-user facing)
     * ------------------------------------------------------------------- */

    /** @return every movie that has at least one showtime, sorted by id. */
    std::vector<Movie> listPlayingMovies() const;

    /** @return every movie known to the catalog, sorted by id. */
    std::vector<Movie> listAllMovies() const;

    /** @return every theater, sorted by id. */
    std::vector<Theater> listTheaters() const;

    /**
     * @brief Look up one movie.
     * @param movieId identifier
     * @throws ServiceError NotFound if there is no such movie.
     */
    Movie getMovie(const MovieId& movieId) const;

    /**
     * @brief Look up one theater.
     * @param theaterId identifier
     * @throws ServiceError NotFound if there is no such theater.
     */
    Theater getTheater(const TheaterId& theaterId) const;

    /**
     * @brief Look up one showtime.
     * @param showtimeId identifier
     * @throws ServiceError NotFound if there is no such showtime.
     */
    Showtime getShowtime(const ShowtimeId& showtimeId) const;

    /**
     * @brief Theaters that screen a given movie.
     * @param movieId identifier
     * @return the theaters, sorted by id; empty if the movie is not screened
     * @throws ServiceError NotFound if the movie itself does not exist.
     */
    std::vector<Theater> listTheatersShowingMovie(const MovieId& movieId) const;

    /**
     * @brief Showtimes of a movie in a theater, ascending by start time.
     * @throws ServiceError NotFound if the movie or the theater is unknown.
     */
    std::vector<Showtime> listShowtimes(const MovieId& movieId,
                                        const TheaterId& theaterId) const;

    /** @brief Every showtime in the catalog, sorted by id. */
    std::vector<Showtime> listAllShowtimes() const;

    /**
     * @brief Free seats for a showtime, in row order.
     * @throws ServiceError NotFound if the showtime is unknown.
     */
    std::vector<SeatLabel> listAvailableSeats(const ShowtimeId& showtimeId) const;

    /**
     * @brief Booked seats for a showtime, in row order.
     * @throws ServiceError NotFound if the showtime is unknown.
     */
    std::vector<SeatLabel> listBookedSeats(const ShowtimeId& showtimeId) const;

    /* ---------------------------------------------------------------------
     * Booking
     * ------------------------------------------------------------------- */

    /**
     * @brief Reserve a set of seats, all or nothing.
     *
     * **This is the method the whole exercise is about.** The entire
     * check-then-act sequence runs inside one critical section:
     *
     *     lock
     *       for each requested seat: must exist and be free   <- check
     *       for each requested seat: mark booked              <- act
     *       create the booking record
     *     unlock
     *     notify the persistence observer
     *
     * Because no other thread can interleave between the check and the act,
     * two concurrent requests for seat "a1" cannot both succeed: one of them
     * finds the seat already marked and gets SeatUnavailable. There is no
     * partial success - if any seat in the request is taken, nothing at all
     * is reserved.
     *
     * @param showtimeId   the screening to book into
     * @param seats        one or more seat labels; duplicates are rejected
     * @param customerName free-form, may be empty
     * @return the confirmed booking, including its generated id
     * @throws ServiceError NotFound if the showtime or a seat label is
     *         unknown, InvalidArgument if @p seats is empty or contains
     *         duplicates, SeatUnavailable if any seat is already booked.
     */
    Booking bookSeats(const ShowtimeId& showtimeId,
                      const std::vector<SeatLabel>& seats,
                      const std::string& customerName);

    /**
     * @brief Look up a booking.
     * @throws ServiceError NotFound if there is no such booking.
     */
    Booking getBooking(const BookingId& bookingId) const;

    /** @return every booking, sorted by id. */
    std::vector<Booking> listBookings() const;

    /**
     * @brief Cancel a booking and free its seats.
     * @param bookingId identifier
     * @throws ServiceError NotFound if there is no such booking.
     */
    void cancelBooking(const BookingId& bookingId);

    /* ---------------------------------------------------------------------
     * Administration - movies
     * ------------------------------------------------------------------- */

    /**
     * @brief Add a movie.
     * @param movie the movie; if @c movie.id is empty an id is generated
     * @return the stored movie, with its final id
     * @throws ServiceError InvalidArgument for an empty title or a
     *         non-positive duration, Conflict if the id is already taken.
     */
    Movie addMovie(const Movie& movie);

    /**
     * @brief Replace a movie's mutable fields. The id cannot change.
     * @throws ServiceError NotFound / InvalidArgument.
     */
    Movie updateMovie(const MovieId& movieId, const Movie& changes);

    /**
     * @brief Remove a movie together with its showtimes and their bookings.
     * @throws ServiceError NotFound if the movie does not exist.
     */
    void removeMovie(const MovieId& movieId);

    /* ---------------------------------------------------------------------
     * Administration - theaters
     * ------------------------------------------------------------------- */

    /**
     * @brief Add a theater.
     *
     * A capacity or row width of 0 means "use the configured default",
     * which is how the "defaults to 20" requirement is honoured for
     * theaters created through the admin API.
     *
     * @throws ServiceError InvalidArgument / Conflict.
     */
    Theater addTheater(const Theater& theater);

    /**
     * @brief Update a theater, propagating any layout change to its showtimes.
     *
     * Layout changes are applied to every showtime of this theater through
     * SeatMap::relayout, so a shrink that would orphan a booked seat is
     * refused and the whole update is rolled back.
     *
     * @throws ServiceError NotFound / InvalidArgument / Conflict.
     */
    Theater updateTheater(const TheaterId& theaterId, const Theater& changes);

    /**
     * @brief Remove a theater together with its showtimes and their bookings.
     * @throws ServiceError NotFound if the theater does not exist.
     */
    void removeTheater(const TheaterId& theaterId);

    /* ---------------------------------------------------------------------
     * Administration - showtimes
     * ------------------------------------------------------------------- */

    /**
     * @brief Add one showtime.
     *
     * The seat layout is taken from the theater. Two showtimes of the same
     * theater may not start at the same minute.
     *
     * @param showtime movie id, theater id and start time must be set; the
     *                 id is generated if empty
     * @throws ServiceError NotFound if the movie or theater is unknown,
     *         Conflict if the theater already has a showtime at that time.
     */
    Showtime addShowtime(const Showtime& showtime);

    /**
     * @brief Change a showtime's start time.
     *
     * Only the start time is mutable. Moving a screening keeps its bookings -
     * the customers keep their seats, the film just starts later.
     *
     * @throws ServiceError NotFound / Conflict.
     */
    Showtime updateShowtimeStart(const ShowtimeId& showtimeId,
                                 MinutesSinceMidnight newStartTime);

    /**
     * @brief Remove a showtime and every booking made against it.
     * @throws ServiceError NotFound if the showtime does not exist.
     */
    void removeShowtime(const ShowtimeId& showtimeId);

    /**
     * @brief Replace a (movie, theater) timeline with the generated default.
     *
     * Existing showtimes for that pair are removed first, together with their
     * bookings - this is destructive and the REST layer says so.
     *
     * @param movieId   the movie
     * @param theaterId the theater
     * @param config    day bounds used by schedule::generateDefaultStartTimes
     * @return the newly created showtimes, ascending by start time
     * @throws ServiceError NotFound if either entity is unknown.
     */
    std::vector<Showtime> applyDefaultSchedule(const MovieId& movieId,
                                               const TheaterId& theaterId,
                                               const ScheduleConfig& config);

private:
    /**
     * @brief A showtime plus its live seat occupancy.
     *
     * Held by value in the map. It is not copyable-out to callers; the
     * queries build plain Showtime copies from it.
     */
    struct ShowtimeRecord {
        Showtime showtime;
        SeatMap  seats;

        ShowtimeRecord(const Showtime& s, const SeatMap& m)
            : showtime(s)
            , seats(m)
        {
        }
    };

    /* ---- helpers that REQUIRE m_mutex to be held by the caller ---------- */

    const ShowtimeRecord& requireShowtimeLocked(const ShowtimeId& id) const;
    ShowtimeRecord&       requireShowtimeLocked(const ShowtimeId& id);
    const Movie&          requireMovieLocked(const MovieId& id) const;
    const Theater&        requireTheaterLocked(const TheaterId& id) const;

    /** @brief Erase a showtime and its bookings. Lock must be held. */
    void eraseShowtimeLocked(const ShowtimeId& id);

    /** @brief Validate a snapshot before installing it. Lock must be held. */
    static CatalogSnapshot validateSnapshot(const CatalogSnapshot& snapshot,
                                            const TheaterDefaults& defaults);

    /**
     * @brief Fire the change callback. Lock must NOT be held.
     *
     * Reads m_listener under m_listenerMutex - a separate, tiny lock so that
     * registering a listener never has to contend with the main data lock.
     */
    void notifyChanged() noexcept;

    /* ---- state ---------------------------------------------------------- */

    /**
     * CRITICAL SECTION: this mutex guards m_movies, m_theaters, m_showtimes
     * and m_bookings. It is `mutable` so that the const query methods can
     * lock it. Never held while calling out of this class.
     */
    mutable std::mutex m_mutex;

    std::map<MovieId, Movie>              m_movies;
    std::map<TheaterId, Theater>          m_theaters;
    std::map<ShowtimeId, ShowtimeRecord>  m_showtimes;
    std::map<BookingId, Booking>          m_bookings;

    TheaterDefaults m_defaults;

    // Id generators carry their own locks; see IdGenerator.
    IdGenerator m_movieIds;
    IdGenerator m_theaterIds;
    IdGenerator m_showtimeIds;
    IdGenerator m_bookingIds;

    /**
     * CRITICAL SECTION: guards m_listener and m_activeNotifications.
     * Paired with m_listenerIdle.
     */
    mutable std::mutex      m_listenerMutex;
    std::condition_variable m_listenerIdle;

    CatalogChangeListener* m_listener = nullptr;

    /**
     * How many onCatalogChanged() callbacks are executing right now.
     *
     * The callback is invoked with m_listenerMutex released (it must be - it
     * may take locks of its own), so without this counter setChangeListener()
     * could return while a callback was still running inside a listener that
     * is about to be destroyed. See setChangeListener() for the drain.
     */
    int m_activeNotifications = 0;
};

} // namespace moviebackend

#endif // MOVIEBACKEND_CATALOG_HPP
