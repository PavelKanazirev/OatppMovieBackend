/**
 * @file Catalog.cpp
 * @brief Implementation of the in-memory catalog.
 *
 * Read the long comment at the top of Catalog.hpp first - it explains the
 * locking strategy that every method here follows.
 *
 * The shape of every public method in this file is the same:
 *
 *     ReturnType Catalog::something(args)
 *     {
 *         ReturnType result;
 *         {
 *             std::lock_guard<std::mutex> guard(m_mutex);   // CRITICAL SECTION
 *             ... work entirely on member state ...
 *         }                                                 // lock released
 *         notifyChanged();   // mutators only, and always outside the lock
 *         return result;
 *     }
 *
 * Deviating from that shape is how this file would acquire a deadlock, so it
 * is worth keeping mechanical.
 */
#include "moviebackend/Catalog.hpp"

#include "moviebackend/Errors.hpp"
#include "moviebackend/Logging.hpp"
#include "moviebackend/Schedule.hpp"

#include <chrono>
#include <set>
#include <utility>

namespace moviebackend {

namespace {

/** @return the current wall-clock time as seconds since the Unix epoch. */
std::int64_t nowEpochSeconds()
{
    const std::chrono::system_clock::time_point now =
        std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(
               now.time_since_epoch())
        .count();
}

/**
 * @brief Apply the configured defaults to a theater that left them at 0.
 *
 * "0 means default" is the convention used by the admin API so that a caller
 * can omit the field entirely and get the required capacity of 20.
 */
Theater withDefaults(const Theater& theater, const TheaterDefaults& defaults)
{
    Theater result = theater;
    if (result.seatCapacity <= 0) {
        result.seatCapacity = defaults.seatCapacity;
    }
    if (result.seatsPerRow <= 0) {
        result.seatsPerRow = defaults.seatsPerRow;
    }
    return result;
}

} // unnamed namespace

/* =========================================================================
 * Construction
 * ========================================================================= */

Catalog::Catalog(const TheaterDefaults& defaults)
    : m_defaults(defaults)
    , m_movieIds("m")
    , m_theaterIds("t")
    , m_showtimeIds("s")
    , m_bookingIds("b")
{
    // Probe the configured defaults immediately. Without this, a config file
    // saying `seat_capacity: 1000, seats_per_row: 10` (100 rows, more than the
    // 26 we can label) is accepted here and then fails much later - at the
    // first theater creation, or during catalog load with a message blaming
    // catalog.json rather than the config field that is actually wrong.
    try {
        const SeatMap probe(m_defaults.seatCapacity, m_defaults.seatsPerRow);
        (void)probe;
    } catch (const ServiceError& error) {
        throw invalidArgument(std::string("theater_defaults in the configuration "
                                          "are not a usable seat layout: ")
                              + error.what());
    }
}

Catalog::~Catalog() = default;

void Catalog::setChangeListener(CatalogChangeListener* listener)
{
    // CRITICAL SECTION: m_listenerMutex only. Kept separate from m_mutex so
    // that attaching or detaching an observer never blocks a booking.
    std::unique_lock<std::mutex> lock(m_listenerMutex);

    m_listener = listener;

    // Then wait for any callback that was already running to finish.
    //
    // Publishing the new pointer first is what makes this terminate: a
    // notifyChanged() that has not yet read m_listener will now read the new
    // value, so only the already-started callbacks are left to drain.
    //
    // Without this wait, ~PersistenceWorker could detach and then destroy
    // itself while a request thread was still inside its onCatalogChanged() -
    // a use-after-free that would reproduce roughly never in testing.
    m_listenerIdle.wait(lock, [this]() { return m_activeNotifications == 0; });
}

void Catalog::notifyChanged() noexcept
{
    CatalogChangeListener* listener = nullptr;
    {
        const std::lock_guard<std::mutex> guard(m_listenerMutex);
        listener = m_listener;
        if (listener == nullptr) {
            return;
        }
        // Announce the in-flight callback before releasing the lock, so a
        // concurrent detach cannot slip past it.
        ++m_activeNotifications;
    }

    // The callback runs with NO lock held - see the header. onCatalogChanged
    // is declared noexcept, so there is nothing to catch here.
    listener->onCatalogChanged();

    {
        const std::lock_guard<std::mutex> guard(m_listenerMutex);
        --m_activeNotifications;
    }
    m_listenerIdle.notify_all();
}

/* =========================================================================
 * Private helpers - the caller must already hold m_mutex
 * ========================================================================= */

const Movie& Catalog::requireMovieLocked(const MovieId& id) const
{
    const std::map<MovieId, Movie>::const_iterator it = m_movies.find(id);
    if (it == m_movies.end()) {
        throw notFound("movie", id);
    }
    return it->second;
}

const Theater& Catalog::requireTheaterLocked(const TheaterId& id) const
{
    const std::map<TheaterId, Theater>::const_iterator it = m_theaters.find(id);
    if (it == m_theaters.end()) {
        throw notFound("theater", id);
    }
    return it->second;
}

const Catalog::ShowtimeRecord& Catalog::requireShowtimeLocked(
    const ShowtimeId& id) const
{
    const std::map<ShowtimeId, ShowtimeRecord>::const_iterator it =
        m_showtimes.find(id);
    if (it == m_showtimes.end()) {
        throw notFound("showtime", id);
    }
    return it->second;
}

Catalog::ShowtimeRecord& Catalog::requireShowtimeLocked(const ShowtimeId& id)
{
    const std::map<ShowtimeId, ShowtimeRecord>::iterator it = m_showtimes.find(id);
    if (it == m_showtimes.end()) {
        throw notFound("showtime", id);
    }
    return it->second;
}

void Catalog::eraseShowtimeLocked(const ShowtimeId& id)
{
    // Bookings belong to a showtime; removing the screening removes them.
    std::map<BookingId, Booking>::iterator it = m_bookings.begin();
    while (it != m_bookings.end()) {
        if (it->second.showtimeId == id) {
            it = m_bookings.erase(it);
        } else {
            ++it;
        }
    }
    m_showtimes.erase(id);
}

/* =========================================================================
 * Bulk load / snapshot
 * ========================================================================= */

CatalogSnapshot Catalog::validateSnapshot(const CatalogSnapshot& snapshot,
                                          const TheaterDefaults& defaults)
{
    // Build the validated copy to one side. Nothing is installed unless this
    // whole function succeeds, which is what gives restore() its all-or-
    // nothing behaviour.
    CatalogSnapshot validated;

    std::set<MovieId>   movieIds;
    std::set<TheaterId> theaterIds;

    for (std::size_t i = 0; i < snapshot.movies.size(); ++i) {
        const Movie& movie = snapshot.movies[i];
        if (movie.id.empty()) {
            throw invalidArgument("catalog contains a movie with an empty id");
        }
        if (movie.durationMinutes <= 0) {
            throw invalidArgument("movie '" + movie.id
                                  + "' has a non-positive duration");
        }
        if (!movieIds.insert(movie.id).second) {
            throw invalidArgument("duplicate movie id '" + movie.id + "'");
        }
        validated.movies.push_back(movie);
    }

    for (std::size_t i = 0; i < snapshot.theaters.size(); ++i) {
        const Theater theater = withDefaults(snapshot.theaters[i], defaults);
        if (theater.id.empty()) {
            throw invalidArgument("catalog contains a theater with an empty id");
        }
        if (!theaterIds.insert(theater.id).second) {
            throw invalidArgument("duplicate theater id '" + theater.id + "'");
        }
        // Constructing a SeatMap validates the layout and throws if the
        // capacity cannot be labelled.
        const SeatMap probe(theater.seatCapacity, theater.seatsPerRow);
        (void)probe;
        validated.theaters.push_back(theater);
    }

    std::set<ShowtimeId> showtimeIds;

    // Keeps the showtime entries reachable while the bookings below are
    // cross-checked against them.
    std::map<ShowtimeId, ShowtimeSnapshot> showtimesById;

    // (theaterId, startTime) pairs, to catch two screenings in one hall at
    // exactly the same minute.
    std::set<std::pair<TheaterId, MinutesSinceMidnight> > occupiedSlots;

    for (std::size_t i = 0; i < snapshot.showtimes.size(); ++i) {
        const ShowtimeSnapshot& entry = snapshot.showtimes[i];
        const Showtime& showtime = entry.showtime;

        if (showtime.id.empty()) {
            throw invalidArgument("catalog contains a showtime with an empty id");
        }
        if (!showtimeIds.insert(showtime.id).second) {
            throw invalidArgument("duplicate showtime id '" + showtime.id + "'");
        }
        if (movieIds.find(showtime.movieId) == movieIds.end()) {
            throw invalidArgument("showtime '" + showtime.id
                                  + "' refers to unknown movie '"
                                  + showtime.movieId + "'");
        }
        if (theaterIds.find(showtime.theaterId) == theaterIds.end()) {
            throw invalidArgument("showtime '" + showtime.id
                                  + "' refers to unknown theater '"
                                  + showtime.theaterId + "'");
        }
        if (showtime.startTime < 0 || showtime.startTime >= 24 * 60) {
            throw invalidArgument("showtime '" + showtime.id
                                  + "' has a start time outside the day");
        }

        const std::pair<TheaterId, MinutesSinceMidnight> slot(
            showtime.theaterId, showtime.startTime);
        if (!occupiedSlots.insert(slot).second) {
            throw invalidArgument("theater '" + showtime.theaterId
                                  + "' has two showtimes starting at "
                                  + schedule::formatTimeOfDay(showtime.startTime));
        }

        // Every booked seat must be a real seat in this showtime's layout.
        SeatMap seats(showtime.seatCapacity, showtime.seatsPerRow);
        for (std::size_t s = 0; s < entry.bookedSeats.size(); ++s) {
            // book() throws SeatUnavailable on a duplicate, which is exactly
            // the validation we want for a file listing "a1" twice.
            seats.book(entry.bookedSeats[s]);
        }

        validated.showtimes.push_back(entry);
        showtimesById.insert(std::make_pair(showtime.id, entry));
    }

    std::set<BookingId> bookingIds;

    // (showtimeId, seatLabel) pairs already claimed by some booking. This is
    // what stops two bookings in the file from owning the same seat.
    std::set<std::pair<ShowtimeId, SeatLabel> > claimedSeats;

    for (std::size_t i = 0; i < snapshot.bookings.size(); ++i) {
        const Booking& booking = snapshot.bookings[i];
        if (booking.id.empty()) {
            throw invalidArgument("catalog contains a booking with an empty id");
        }
        if (!bookingIds.insert(booking.id).second) {
            throw invalidArgument("duplicate booking id '" + booking.id + "'");
        }

        const std::map<ShowtimeId, ShowtimeSnapshot>::const_iterator showtimeIt =
            showtimesById.find(booking.showtimeId);
        if (showtimeIt == showtimesById.end()) {
            throw invalidArgument("booking '" + booking.id
                                  + "' refers to unknown showtime '"
                                  + booking.showtimeId + "'");
        }
        if (booking.seats.empty()) {
            throw invalidArgument("booking '" + booking.id + "' reserves no seats");
        }

        // The seat map and the booking ledger are two views of the same fact
        // and must agree. Loading a file where they disagree would reintroduce
        // exactly the bug this service exists to prevent: two bookings holding
        // seat "a1", where cancelling one frees a seat the other still owns.
        //
        // Note the check is one-directional. A showtime may legitimately mark
        // a seat booked with no booking record behind it (the seed catalog
        // does this to represent a pre-sold house); what is forbidden is a
        // booking claiming a seat that is not marked, or claimed twice.
        SeatMap layout(showtimeIt->second.showtime.seatCapacity,
                       showtimeIt->second.showtime.seatsPerRow);

        std::set<SeatLabel> markedBooked;
        for (std::size_t s = 0; s < showtimeIt->second.bookedSeats.size(); ++s) {
            markedBooked.insert(showtimeIt->second.bookedSeats[s]);
        }

        for (std::size_t s = 0; s < booking.seats.size(); ++s) {
            const SeatLabel& seat = booking.seats[s];

            const std::optional<std::size_t> index = layout.labelToIndex(seat);
            if (!index.has_value()) {
                throw invalidArgument("booking '" + booking.id + "' claims seat '"
                                      + seat + "', which does not exist in showtime '"
                                      + booking.showtimeId + "'");
            }

            // Compare canonical labels, so "A1" and "a1" cannot slip past as
            // two different seats.
            const SeatLabel canonical = layout.indexToLabel(*index);

            if (markedBooked.find(canonical) == markedBooked.end()
                && markedBooked.find(seat) == markedBooked.end()) {
                throw invalidArgument("booking '" + booking.id + "' claims seat '"
                                      + seat + "' but showtime '" + booking.showtimeId
                                      + "' does not list it as booked");
            }

            const std::pair<ShowtimeId, SeatLabel> claim(booking.showtimeId, canonical);
            if (!claimedSeats.insert(claim).second) {
                throw invalidArgument("seat '" + seat + "' in showtime '"
                                      + booking.showtimeId
                                      + "' is claimed by more than one booking");
            }
        }

        validated.bookings.push_back(booking);
    }

    return validated;
}

void Catalog::restore(const CatalogSnapshot& snapshot)
{
    // Validate *before* taking the lock. Validation is pure and can be slow;
    // there is no reason to block bookings while it runs, and if it throws
    // the catalog was never touched.
    const CatalogSnapshot validated = validateSnapshot(snapshot, m_defaults);

    {
        // CRITICAL SECTION: swap the entire world in one go, so no request
        // thread can ever observe a half-loaded catalog.
        const std::lock_guard<std::mutex> guard(m_mutex);

        m_movies.clear();
        m_theaters.clear();
        m_showtimes.clear();
        m_bookings.clear();

        for (std::size_t i = 0; i < validated.movies.size(); ++i) {
            m_movies.insert(std::make_pair(validated.movies[i].id,
                                           validated.movies[i]));
            m_movieIds.observeExistingId(validated.movies[i].id);
        }

        for (std::size_t i = 0; i < validated.theaters.size(); ++i) {
            m_theaters.insert(std::make_pair(validated.theaters[i].id,
                                             validated.theaters[i]));
            m_theaterIds.observeExistingId(validated.theaters[i].id);
        }

        for (std::size_t i = 0; i < validated.showtimes.size(); ++i) {
            const ShowtimeSnapshot& entry = validated.showtimes[i];

            SeatMap seats(entry.showtime.seatCapacity, entry.showtime.seatsPerRow);
            for (std::size_t s = 0; s < entry.bookedSeats.size(); ++s) {
                seats.book(entry.bookedSeats[s]);
            }

            m_showtimes.insert(std::make_pair(
                entry.showtime.id, ShowtimeRecord(entry.showtime, seats)));
            m_showtimeIds.observeExistingId(entry.showtime.id);
        }

        for (std::size_t i = 0; i < validated.bookings.size(); ++i) {
            m_bookings.insert(std::make_pair(validated.bookings[i].id,
                                             validated.bookings[i]));
            m_bookingIds.observeExistingId(validated.bookings[i].id);
        }
    }

    MB_LOG_INFO("catalog restored: {} movies, {} theaters, {} showtimes, {} bookings",
                validated.movies.size(), validated.theaters.size(),
                validated.showtimes.size(), validated.bookings.size());

    // restore() is itself a change, so the persistence worker is told - which
    // is harmless when the change came from the store in the first place
    // (it just rewrites an identical file once).
    notifyChanged();
}

CatalogSnapshot Catalog::snapshot() const
{
    CatalogSnapshot result;

    // CRITICAL SECTION: the longest one in the system - a copy of every
    // container. Still only memory traffic; no I/O happens under the lock.
    const std::lock_guard<std::mutex> guard(m_mutex);

    result.movies.reserve(m_movies.size());
    for (std::map<MovieId, Movie>::const_iterator it = m_movies.begin();
         it != m_movies.end(); ++it) {
        result.movies.push_back(it->second);
    }

    result.theaters.reserve(m_theaters.size());
    for (std::map<TheaterId, Theater>::const_iterator it = m_theaters.begin();
         it != m_theaters.end(); ++it) {
        result.theaters.push_back(it->second);
    }

    result.showtimes.reserve(m_showtimes.size());
    for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
         it != m_showtimes.end(); ++it) {
        ShowtimeSnapshot entry;
        entry.showtime = it->second.showtime;
        entry.bookedSeats = it->second.seats.bookedSeats();
        result.showtimes.push_back(entry);
    }

    result.bookings.reserve(m_bookings.size());
    for (std::map<BookingId, Booking>::const_iterator it = m_bookings.begin();
         it != m_bookings.end(); ++it) {
        result.bookings.push_back(it->second);
    }

    return result;
}

/* =========================================================================
 * Queries
 * ========================================================================= */

std::vector<Movie> Catalog::listPlayingMovies() const
{
    std::vector<Movie> result;

    const std::lock_guard<std::mutex> guard(m_mutex);

    // "Playing" = has at least one showtime. Collect the ids that appear in
    // the showtime map, then emit those movies in id order (std::map already
    // iterates sorted, so the output is deterministic - which the smoke tests
    // depend on).
    std::set<MovieId> playing;
    for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
         it != m_showtimes.end(); ++it) {
        playing.insert(it->second.showtime.movieId);
    }

    for (std::map<MovieId, Movie>::const_iterator it = m_movies.begin();
         it != m_movies.end(); ++it) {
        if (playing.find(it->first) != playing.end()) {
            result.push_back(it->second);
        }
    }
    return result;
}

std::vector<Movie> Catalog::listAllMovies() const
{
    std::vector<Movie> result;

    const std::lock_guard<std::mutex> guard(m_mutex);

    result.reserve(m_movies.size());
    for (std::map<MovieId, Movie>::const_iterator it = m_movies.begin();
         it != m_movies.end(); ++it) {
        result.push_back(it->second);
    }
    return result;
}

std::vector<Theater> Catalog::listTheaters() const
{
    std::vector<Theater> result;

    const std::lock_guard<std::mutex> guard(m_mutex);

    result.reserve(m_theaters.size());
    for (std::map<TheaterId, Theater>::const_iterator it = m_theaters.begin();
         it != m_theaters.end(); ++it) {
        result.push_back(it->second);
    }
    return result;
}

Movie Catalog::getMovie(const MovieId& movieId) const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return requireMovieLocked(movieId);   // returns a copy
}

Theater Catalog::getTheater(const TheaterId& theaterId) const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return requireTheaterLocked(theaterId);
}

Showtime Catalog::getShowtime(const ShowtimeId& showtimeId) const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return requireShowtimeLocked(showtimeId).showtime;
}

std::vector<Theater> Catalog::listTheatersShowingMovie(const MovieId& movieId) const
{
    std::vector<Theater> result;

    const std::lock_guard<std::mutex> guard(m_mutex);

    // Asking about a movie that does not exist is a 404, not an empty list -
    // the client should be able to tell "no such film" from "not on anywhere".
    requireMovieLocked(movieId);

    std::set<TheaterId> theaterIds;
    for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
         it != m_showtimes.end(); ++it) {
        if (it->second.showtime.movieId == movieId) {
            theaterIds.insert(it->second.showtime.theaterId);
        }
    }

    for (std::map<TheaterId, Theater>::const_iterator it = m_theaters.begin();
         it != m_theaters.end(); ++it) {
        if (theaterIds.find(it->first) != theaterIds.end()) {
            result.push_back(it->second);
        }
    }
    return result;
}

std::vector<Showtime> Catalog::listShowtimes(const MovieId& movieId,
                                             const TheaterId& theaterId) const
{
    std::vector<Showtime> result;

    const std::lock_guard<std::mutex> guard(m_mutex);

    requireMovieLocked(movieId);
    requireTheaterLocked(theaterId);

    for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
         it != m_showtimes.end(); ++it) {
        if (it->second.showtime.movieId == movieId
            && it->second.showtime.theaterId == theaterId) {
            result.push_back(it->second.showtime);
        }
    }

    // Sorted by start time - a timeline is only useful in order. Written as a
    // plain insertion into a sorted position rather than std::sort with a
    // comparator lambda, in keeping with the project's style.
    for (std::size_t i = 1; i < result.size(); ++i) {
        const Showtime key = result[i];
        std::size_t j = i;
        while (j > 0 && result[j - 1].startTime > key.startTime) {
            result[j] = result[j - 1];
            --j;
        }
        result[j] = key;
    }

    return result;
}

std::vector<Showtime> Catalog::listAllShowtimes() const
{
    std::vector<Showtime> result;

    const std::lock_guard<std::mutex> guard(m_mutex);

    result.reserve(m_showtimes.size());
    for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
         it != m_showtimes.end(); ++it) {
        result.push_back(it->second.showtime);
    }
    return result;
}

std::vector<SeatLabel> Catalog::listAvailableSeats(const ShowtimeId& showtimeId) const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return requireShowtimeLocked(showtimeId).seats.availableSeats();
}

std::vector<SeatLabel> Catalog::listBookedSeats(const ShowtimeId& showtimeId) const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return requireShowtimeLocked(showtimeId).seats.bookedSeats();
}

/* =========================================================================
 * Booking - the over-booking guard
 * ========================================================================= */

Booking Catalog::bookSeats(const ShowtimeId& showtimeId,
                           const std::vector<SeatLabel>& seats,
                           const std::string& customerName)
{
    if (seats.empty()) {
        throw invalidArgument("a booking must reserve at least one seat");
    }

    Booking booking;

    {
        /* -----------------------------------------------------------------
         * CRITICAL SECTION - the whole point of the exercise.
         *
         * Check and act happen without releasing the lock in between, so two
         * threads asking for the same seat cannot both pass the check. The
         * loser sees the seat already marked Booked and gets SeatUnavailable.
         * ----------------------------------------------------------------- */
        const std::lock_guard<std::mutex> guard(m_mutex);

        ShowtimeRecord& record = requireShowtimeLocked(showtimeId);

        // --- Phase 1: validate every seat before changing anything ---------
        //
        // Doing all the checking first means the mutation phase below cannot
        // fail halfway through and leave a partially applied booking.

        std::set<SeatLabel> requested;
        for (std::size_t i = 0; i < seats.size(); ++i) {
            const SeatLabel& label = seats[i];

            const std::optional<std::size_t> index =
                record.seats.labelToIndex(label);
            if (!index.has_value()) {
                throw notFound("seat", label);
            }

            // Reject "a1" twice in one request: it is almost certainly a
            // client bug, and silently collapsing it would make the seat
            // count in the response disagree with the request.
            if (!requested.insert(record.seats.indexToLabel(*index)).second) {
                throw invalidArgument("seat '" + label
                                      + "' appears more than once in the request");
            }

            if (!record.seats.isFree(label)) {
                throw ServiceError(ErrorCode::SeatUnavailable,
                                   "seat '" + label + "' is already booked");
            }
        }

        // --- Phase 2: commit ----------------------------------------------
        //
        // Every seat was verified free a few lines ago and the lock has not
        // been released, so none of these calls can throw.

        booking.id = m_bookingIds.next();
        booking.showtimeId = showtimeId;
        booking.customerName = customerName;
        booking.createdAtEpochSeconds = nowEpochSeconds();

        for (std::set<SeatLabel>::const_iterator it = requested.begin();
             it != requested.end(); ++it) {
            record.seats.book(*it);
            booking.seats.push_back(*it);
        }

        m_bookings.insert(std::make_pair(booking.id, booking));
    }
    // ---- lock released ---------------------------------------------------

    MB_LOG_INFO("booking {} confirmed: {} seat(s) for showtime {}",
                booking.id, booking.seats.size(), showtimeId);

    notifyChanged();
    return booking;
}

Booking Catalog::getBooking(const BookingId& bookingId) const
{
    const std::lock_guard<std::mutex> guard(m_mutex);

    const std::map<BookingId, Booking>::const_iterator it = m_bookings.find(bookingId);
    if (it == m_bookings.end()) {
        throw notFound("booking", bookingId);
    }
    return it->second;
}

std::vector<Booking> Catalog::listBookings() const
{
    std::vector<Booking> result;

    const std::lock_guard<std::mutex> guard(m_mutex);

    result.reserve(m_bookings.size());
    for (std::map<BookingId, Booking>::const_iterator it = m_bookings.begin();
         it != m_bookings.end(); ++it) {
        result.push_back(it->second);
    }
    return result;
}

void Catalog::cancelBooking(const BookingId& bookingId)
{
    {
        // CRITICAL SECTION: freeing the seats and dropping the booking record
        // must be one indivisible step, or a seat could be double-freed.
        const std::lock_guard<std::mutex> guard(m_mutex);

        const std::map<BookingId, Booking>::iterator it = m_bookings.find(bookingId);
        if (it == m_bookings.end()) {
            throw notFound("booking", bookingId);
        }

        const std::map<ShowtimeId, ShowtimeRecord>::iterator showtimeIt =
            m_showtimes.find(it->second.showtimeId);

        if (showtimeIt != m_showtimes.end()) {
            for (std::size_t i = 0; i < it->second.seats.size(); ++i) {
                const SeatLabel& seat = it->second.seats[i];

                // Skip anything that is not a seat of this showtime instead of
                // letting release() throw. A throw here would abandon the
                // cancellation half-done - some seats freed, the booking record
                // still present and now permanently uncancellable. The seats a
                // booking holds are validated on load and on creation, so this
                // can only trigger on state that got past validation; degrading
                // to "free what we can" is strictly better than corrupting.
                if (showtimeIt->second.seats.labelToIndex(seat).has_value()) {
                    showtimeIt->second.seats.release(seat);
                } else {
                    MB_LOG_WARN("booking {} holds seat '{}', which is not part of "
                                "showtime {} - ignoring it while cancelling",
                                bookingId, seat, it->second.showtimeId);
                }
            }
        }

        m_bookings.erase(it);
    }

    MB_LOG_INFO("booking {} cancelled", bookingId);
    notifyChanged();
}

/* =========================================================================
 * Administration - movies
 * ========================================================================= */

Movie Catalog::addMovie(const Movie& movie)
{
    if (movie.title.empty()) {
        throw invalidArgument("movie title must not be empty");
    }
    if (movie.durationMinutes <= 0) {
        throw invalidArgument("movie duration must be positive");
    }

    Movie stored = movie;

    {
        const std::lock_guard<std::mutex> guard(m_mutex);

        if (stored.id.empty()) {
            stored.id = m_movieIds.next();
        } else if (m_movies.find(stored.id) != m_movies.end()) {
            throw conflict("movie id '" + stored.id + "' already exists");
        } else {
            m_movieIds.observeExistingId(stored.id);
        }

        m_movies.insert(std::make_pair(stored.id, stored));
    }

    MB_LOG_INFO("movie {} added: '{}'", stored.id, stored.title);
    notifyChanged();
    return stored;
}

Movie Catalog::updateMovie(const MovieId& movieId, const Movie& changes)
{
    if (changes.title.empty()) {
        throw invalidArgument("movie title must not be empty");
    }
    if (changes.durationMinutes <= 0) {
        throw invalidArgument("movie duration must be positive");
    }

    Movie stored;

    {
        const std::lock_guard<std::mutex> guard(m_mutex);

        const std::map<MovieId, Movie>::iterator it = m_movies.find(movieId);
        if (it == m_movies.end()) {
            throw notFound("movie", movieId);
        }

        // The id is immutable - changing it would orphan every showtime that
        // points at it. Everything else is replaced wholesale.
        it->second.title = changes.title;
        it->second.durationMinutes = changes.durationMinutes;
        it->second.language = changes.language;
        it->second.genre = changes.genre;

        stored = it->second;
    }

    MB_LOG_INFO("movie {} updated", movieId);
    notifyChanged();
    return stored;
}

void Catalog::removeMovie(const MovieId& movieId)
{
    {
        const std::lock_guard<std::mutex> guard(m_mutex);

        if (m_movies.find(movieId) == m_movies.end()) {
            throw notFound("movie", movieId);
        }

        // Collect first, erase second: erasing from m_showtimes while
        // iterating it would invalidate the iterator.
        std::vector<ShowtimeId> doomed;
        for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
             it != m_showtimes.end(); ++it) {
            if (it->second.showtime.movieId == movieId) {
                doomed.push_back(it->first);
            }
        }
        for (std::size_t i = 0; i < doomed.size(); ++i) {
            eraseShowtimeLocked(doomed[i]);
        }

        m_movies.erase(movieId);
    }

    MB_LOG_INFO("movie {} removed", movieId);
    notifyChanged();
}

/* =========================================================================
 * Administration - theaters
 * ========================================================================= */

Theater Catalog::addTheater(const Theater& theater)
{
    if (theater.name.empty()) {
        throw invalidArgument("theater name must not be empty");
    }

    Theater stored = withDefaults(theater, m_defaults);

    // Validating the layout outside the lock keeps the critical section free
    // of anything that can throw for a reason unrelated to shared state.
    const SeatMap probe(stored.seatCapacity, stored.seatsPerRow);
    (void)probe;

    {
        const std::lock_guard<std::mutex> guard(m_mutex);

        if (stored.id.empty()) {
            stored.id = m_theaterIds.next();
        } else if (m_theaters.find(stored.id) != m_theaters.end()) {
            throw conflict("theater id '" + stored.id + "' already exists");
        } else {
            m_theaterIds.observeExistingId(stored.id);
        }

        m_theaters.insert(std::make_pair(stored.id, stored));
    }

    MB_LOG_INFO("theater {} added: '{}' ({} seats)",
                stored.id, stored.name, stored.seatCapacity);
    notifyChanged();
    return stored;
}

Theater Catalog::updateTheater(const TheaterId& theaterId, const Theater& changes)
{
    if (changes.name.empty()) {
        throw invalidArgument("theater name must not be empty");
    }

    Theater stored;

    {
        const std::lock_guard<std::mutex> guard(m_mutex);

        const std::map<TheaterId, Theater>::iterator it = m_theaters.find(theaterId);
        if (it == m_theaters.end()) {
            throw notFound("theater", theaterId);
        }

        Theater updated = it->second;
        updated.name = changes.name;
        updated.city = changes.city;
        if (changes.seatCapacity > 0) {
            updated.seatCapacity = changes.seatCapacity;
        }
        if (changes.seatsPerRow > 0) {
            updated.seatsPerRow = changes.seatsPerRow;
        }

        const bool layoutChanged =
            updated.seatCapacity != it->second.seatCapacity
            || updated.seatsPerRow != it->second.seatsPerRow;

        if (layoutChanged) {
            // Re-lay-out every screening of this theater. Build all of the new
            // seat maps to one side first: if any of them refuses (because a
            // booked seat would vanish) we throw before touching live state,
            // so the update is all-or-nothing.
            std::vector<std::pair<ShowtimeId, SeatMap> > relaidOut;

            for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator sIt = m_showtimes.begin();
                 sIt != m_showtimes.end(); ++sIt) {
                if (sIt->second.showtime.theaterId != theaterId) {
                    continue;
                }
                SeatMap candidate = sIt->second.seats;
                candidate.relayout(updated.seatCapacity, updated.seatsPerRow);
                relaidOut.push_back(std::make_pair(sIt->first, candidate));
            }

            // Nothing threw - commit.
            for (std::size_t i = 0; i < relaidOut.size(); ++i) {
                ShowtimeRecord& record = m_showtimes.at(relaidOut[i].first);
                record.seats = relaidOut[i].second;
                record.showtime.seatCapacity = updated.seatCapacity;
                record.showtime.seatsPerRow = updated.seatsPerRow;
            }
        }

        it->second = updated;
        stored = updated;
    }

    MB_LOG_INFO("theater {} updated", theaterId);
    notifyChanged();
    return stored;
}

void Catalog::removeTheater(const TheaterId& theaterId)
{
    {
        const std::lock_guard<std::mutex> guard(m_mutex);

        if (m_theaters.find(theaterId) == m_theaters.end()) {
            throw notFound("theater", theaterId);
        }

        std::vector<ShowtimeId> doomed;
        for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
             it != m_showtimes.end(); ++it) {
            if (it->second.showtime.theaterId == theaterId) {
                doomed.push_back(it->first);
            }
        }
        for (std::size_t i = 0; i < doomed.size(); ++i) {
            eraseShowtimeLocked(doomed[i]);
        }

        m_theaters.erase(theaterId);
    }

    MB_LOG_INFO("theater {} removed", theaterId);
    notifyChanged();
}

/* =========================================================================
 * Administration - showtimes
 * ========================================================================= */

Showtime Catalog::addShowtime(const Showtime& showtime)
{
    Showtime stored;

    {
        const std::lock_guard<std::mutex> guard(m_mutex);

        requireMovieLocked(showtime.movieId);
        const Theater& theater = requireTheaterLocked(showtime.theaterId);

        if (showtime.startTime < 0 || showtime.startTime >= 24 * 60) {
            throw invalidArgument("showtime start time is outside the day");
        }

        // One hall cannot run two films at the same minute.
        for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
             it != m_showtimes.end(); ++it) {
            if (it->second.showtime.theaterId == showtime.theaterId
                && it->second.showtime.startTime == showtime.startTime) {
                throw conflict("theater '" + showtime.theaterId
                               + "' already has a showtime at "
                               + schedule::formatTimeOfDay(showtime.startTime));
            }
        }

        stored = showtime;
        // The seat layout always comes from the theater, never from the
        // request - that keeps a hall's screenings consistent with each other.
        stored.seatCapacity = theater.seatCapacity;
        stored.seatsPerRow = theater.seatsPerRow;

        if (stored.id.empty()) {
            stored.id = m_showtimeIds.next();
        } else if (m_showtimes.find(stored.id) != m_showtimes.end()) {
            throw conflict("showtime id '" + stored.id + "' already exists");
        } else {
            m_showtimeIds.observeExistingId(stored.id);
        }

        const SeatMap seats(stored.seatCapacity, stored.seatsPerRow);
        m_showtimes.insert(std::make_pair(stored.id, ShowtimeRecord(stored, seats)));
    }

    MB_LOG_INFO("showtime {} added: movie {} in theater {} at {}",
                stored.id, stored.movieId, stored.theaterId,
                schedule::formatTimeOfDay(stored.startTime));
    notifyChanged();
    return stored;
}

Showtime Catalog::updateShowtimeStart(const ShowtimeId& showtimeId,
                                      MinutesSinceMidnight newStartTime)
{
    if (newStartTime < 0 || newStartTime >= 24 * 60) {
        throw invalidArgument("showtime start time is outside the day");
    }

    Showtime stored;

    {
        const std::lock_guard<std::mutex> guard(m_mutex);

        ShowtimeRecord& record = requireShowtimeLocked(showtimeId);

        for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
             it != m_showtimes.end(); ++it) {
            if (it->first != showtimeId
                && it->second.showtime.theaterId == record.showtime.theaterId
                && it->second.showtime.startTime == newStartTime) {
                throw conflict("theater '" + record.showtime.theaterId
                               + "' already has a showtime at "
                               + schedule::formatTimeOfDay(newStartTime));
            }
        }

        // Bookings are intentionally kept: the audience keeps its seats, the
        // screening simply moves.
        record.showtime.startTime = newStartTime;
        stored = record.showtime;
    }

    MB_LOG_INFO("showtime {} moved to {}",
                showtimeId, schedule::formatTimeOfDay(newStartTime));
    notifyChanged();
    return stored;
}

void Catalog::removeShowtime(const ShowtimeId& showtimeId)
{
    {
        const std::lock_guard<std::mutex> guard(m_mutex);

        if (m_showtimes.find(showtimeId) == m_showtimes.end()) {
            throw notFound("showtime", showtimeId);
        }
        eraseShowtimeLocked(showtimeId);
    }

    MB_LOG_INFO("showtime {} removed", showtimeId);
    notifyChanged();
}

std::vector<Showtime> Catalog::applyDefaultSchedule(const MovieId& movieId,
                                                    const TheaterId& theaterId,
                                                    const ScheduleConfig& config)
{
    std::vector<Showtime> created;

    {
        // CRITICAL SECTION: replacing a whole timeline must be atomic, or a
        // client could see the old screenings deleted and the new ones not
        // yet present.
        const std::lock_guard<std::mutex> guard(m_mutex);

        const Movie& movie = requireMovieLocked(movieId);
        const Theater& theater = requireTheaterLocked(theaterId);

        const std::vector<MinutesSinceMidnight> startTimes =
            schedule::generateDefaultStartTimes(movie.durationMinutes, config);

        // Drop the existing timeline for this pair, bookings included.
        std::vector<ShowtimeId> doomed;
        for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
             it != m_showtimes.end(); ++it) {
            if (it->second.showtime.movieId == movieId
                && it->second.showtime.theaterId == theaterId) {
                doomed.push_back(it->first);
            }
        }
        for (std::size_t i = 0; i < doomed.size(); ++i) {
            eraseShowtimeLocked(doomed[i]);
        }

        // A slot generated for this movie may clash with another movie already
        // scheduled in the same hall. Skipping the clash rather than failing
        // the whole call is the friendlier behaviour: the administrator gets
        // the slots that were actually free.
        for (std::size_t i = 0; i < startTimes.size(); ++i) {
            bool occupied = false;
            for (std::map<ShowtimeId, ShowtimeRecord>::const_iterator it = m_showtimes.begin();
                 it != m_showtimes.end(); ++it) {
                if (it->second.showtime.theaterId == theaterId
                    && it->second.showtime.startTime == startTimes[i]) {
                    occupied = true;
                    break;
                }
            }
            if (occupied) {
                MB_LOG_WARN("default schedule: skipping {} in theater {} - slot taken",
                            schedule::formatTimeOfDay(startTimes[i]), theaterId);
                continue;
            }

            Showtime showtime;
            showtime.id = m_showtimeIds.next();
            showtime.movieId = movieId;
            showtime.theaterId = theaterId;
            showtime.startTime = startTimes[i];
            showtime.seatCapacity = theater.seatCapacity;
            showtime.seatsPerRow = theater.seatsPerRow;

            const SeatMap seats(showtime.seatCapacity, showtime.seatsPerRow);
            m_showtimes.insert(std::make_pair(showtime.id,
                                              ShowtimeRecord(showtime, seats)));
            created.push_back(showtime);
        }
    }

    MB_LOG_INFO("default schedule applied: {} showtimes for movie {} in theater {}",
                created.size(), movieId, theaterId);
    notifyChanged();
    return created;
}

} // namespace moviebackend
