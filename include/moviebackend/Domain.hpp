/**
 * @file Domain.hpp
 * @brief The value types that make up the booking domain.
 *
 * ## Shape of the model (an explicit design decision)
 *
 * A *Movie* is a global entity - "The Silent Protocol" is the same film
 * everywhere. A *Theater* is a global entity too. The thing that actually
 * gets booked is a **Showtime**: the triple (movie, theater, start time).
 * A Showtime owns its own seat map, because seat `a1` in the 09:00 screening
 * has nothing to do with seat `a1` in the 11:00 screening.
 *
 * That is why "which theaters show movie X" is answered by scanning
 * showtimes rather than by a stored many-to-many table - there is only one
 * place where the truth lives, so it cannot go out of sync.
 *
 * ## Style note
 *
 * These are deliberately plain aggregates: public data, no invariants, no
 * behaviour. All the invariants live in Catalog, which is the type that owns
 * them and the lock that protects them. Keeping the value types dumb is what
 * makes it safe to hand copies out of the lock.
 */
#ifndef MOVIEBACKEND_DOMAIN_HPP
#define MOVIEBACKEND_DOMAIN_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace moviebackend {

/* -------------------------------------------------------------------------
 * Identifier aliases.
 *
 * These are all std::string. They are aliased rather than wrapped in distinct
 * strong types on purpose: strong typedefs would be the "correct" modern
 * answer, but they add a layer of template machinery that buys little in a
 * codebase this size and would obscure the parts that actually matter.
 * ------------------------------------------------------------------------- */

/** @brief Identifier of a Movie, e.g. "m1". */
using MovieId = std::string;

/** @brief Identifier of a Theater, e.g. "t1". */
using TheaterId = std::string;

/** @brief Identifier of a Showtime, e.g. "s1". */
using ShowtimeId = std::string;

/** @brief Identifier of a Booking, e.g. "b1". */
using BookingId = std::string;

/** @brief A seat label such as "a1", "b7". Row letter + 1-based seat number. */
using SeatLabel = std::string;

/**
 * @brief A wall-clock time of day expressed as minutes since midnight.
 *
 * The whole service operates on a single, unnamed "business day"; there are
 * no dates anywhere. That is a simplification the requirements allow, and it
 * removes an entire class of timezone problems from a study project.
 *
 * 09:00 is 540, 21:00 is 1260.
 */
using MinutesSinceMidnight = int;

/** @brief A film that can be screened. */
struct Movie {
    MovieId     id;
    std::string title;

    /** Running time in minutes. Drives default showtime generation. */
    int         durationMinutes = 0;

    std::string language;
    std::string genre;
};

/** @brief A venue with a fixed seating layout. */
struct Theater {
    TheaterId   id;
    std::string name;
    std::string city;

    /**
     * Total number of seats. Defaults to 20 (the value required by the
     * exercise) but is overridable per theater and via config.json.
     */
    int         seatCapacity = 20;

    /**
     * Seats per row, which decides the labelling. With capacity 20 and 10
     * per row you get a1..a10 and b1..b10.
     */
    int         seatsPerRow = 10;
};

/**
 * @brief One screening of one movie in one theater at one time.
 *
 * This is the bookable unit. The capacity is copied from the theater when
 * the showtime is created, so that changing a theater later cannot silently
 * invalidate seats somebody has already paid for. AdminService applies
 * capacity changes explicitly and refuses shrinks that would orphan a
 * booked seat.
 */
struct Showtime {
    ShowtimeId          id;
    MovieId             movieId;
    TheaterId           theaterId;
    MinutesSinceMidnight startTime = 0;
    int                 seatCapacity = 20;
    int                 seatsPerRow = 10;
};

/** @brief A confirmed reservation of one or more seats in one showtime. */
struct Booking {
    BookingId              id;
    ShowtimeId             showtimeId;

    /** The seats actually reserved. Never empty for a stored booking. */
    std::vector<SeatLabel> seats;

    std::string            customerName;

    /**
     * Creation time as seconds since the Unix epoch.
     *
     * Stored as a plain integer rather than a std::chrono::time_point so that
     * it serialises to JSON without any calendar formatting decisions.
     */
    std::int64_t           createdAtEpochSeconds = 0;
};

/* -------------------------------------------------------------------------
 * Snapshot types.
 *
 * Catalog hands out a CatalogSnapshot to anything that needs to see the whole
 * world at once - today that is only the JSON persistence layer. The snapshot
 * is taken under the catalog lock and is a pure value afterwards, so the
 * writer thread can serialise it at its leisure without holding anything.
 * This is the reason the persistence layer needs no knowledge of the mutex.
 * ------------------------------------------------------------------------- */

/** @brief A showtime plus the seats that were booked at snapshot time. */
struct ShowtimeSnapshot {
    Showtime               showtime;
    std::vector<SeatLabel> bookedSeats;
};

/** @brief A complete, self-consistent copy of the catalog state. */
struct CatalogSnapshot {
    std::vector<Movie>            movies;
    std::vector<Theater>          theaters;
    std::vector<ShowtimeSnapshot> showtimes;
    std::vector<Booking>          bookings;
};

} // namespace moviebackend

#endif // MOVIEBACKEND_DOMAIN_HPP
