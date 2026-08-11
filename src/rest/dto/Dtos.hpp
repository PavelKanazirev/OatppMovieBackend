/**
 * @file Dtos.hpp
 * @brief The wire format of the REST API.
 *
 * =========================================================================
 *  WHY DTOs EXIST AT ALL, WHEN THE DOMAIN TYPES LOOK SO SIMILAR
 * =========================================================================
 *
 * It is tempting to serialise moviebackend::Movie straight onto the wire and
 * delete this file. Two reasons not to:
 *
 *  1. **They change for different reasons.** The domain type changes when the
 *     business rules change; the wire type changes when a client needs a new
 *     field or a renamed one. Coupling them means an internal refactor is an
 *     API break.
 *
 *  2. **Representations genuinely differ.** A Showtime carries its start time
 *     as minutes-since-midnight because that is what arithmetic wants; the
 *     wire carries "09:00" because that is what a human reading JSON wants.
 *     ShowtimeDto also carries derived availability counts that no domain
 *     type has.
 *
 * Field names are snake_case to match the on-disk catalog format, so an
 * administrator sees the same spelling in the file and in the API.
 *
 * Modern C++ note: oatpp's DTOs are built from macros (DTO_INIT, DTO_FIELD)
 * that generate reflection data at compile time. The `oatpp::String`,
 * `oatpp::Int32` and `oatpp::Vector` types are nullable wrappers, not the
 * std:: types - assigning std::string to oatpp::String works implicitly, and
 * an unassigned field serialises as JSON null.
 */
#ifndef MOVIEBACKEND_DTOS_HPP
#define MOVIEBACKEND_DTOS_HPP

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

namespace moviebackend {
namespace rest {

// Everything between OATPP_CODEGEN_BEGIN(DTO) and OATPP_CODEGEN_END(DTO) may
// use the DTO_* macros. The END is mandatory - it undefines them again so
// they cannot leak into unrelated headers.
#include OATPP_CODEGEN_BEGIN(DTO)

/* =========================================================================
 * Error payload
 * ========================================================================= */

/**
 * @brief The body returned with every 4xx/5xx response.
 *
 * Produced in exactly one place - RestErrorHandler - so that every failure in
 * the service looks the same to a client.
 */
class ErrorDto : public oatpp::DTO {
    DTO_INIT(ErrorDto, DTO)

    /** Machine-readable code, e.g. "SEAT_UNAVAILABLE". */
    DTO_FIELD(String, code);

    /** Human-readable explanation, safe to display. */
    DTO_FIELD(String, message);
};

/* =========================================================================
 * Read models
 * ========================================================================= */

/** @brief A film. */
class MovieDto : public oatpp::DTO {
    DTO_INIT(MovieDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, title);
    DTO_FIELD(Int32,  durationMinutes, "duration_minutes");
    DTO_FIELD(String, language);
    DTO_FIELD(String, genre);
};

/** @brief A venue. */
class TheaterDto : public oatpp::DTO {
    DTO_INIT(TheaterDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, name);
    DTO_FIELD(String, city);
    DTO_FIELD(Int32,  seatCapacity, "seat_capacity");
    DTO_FIELD(Int32,  seatsPerRow, "seats_per_row");
};

/**
 * @brief One screening.
 *
 * `start_time` is a "HH:MM" string rather than the domain's integer, and the
 * two availability counts are derived - the clearest illustration of why the
 * wire type is not the domain type.
 */
class ShowtimeDto : public oatpp::DTO {
    DTO_INIT(ShowtimeDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, movieId, "movie_id");
    DTO_FIELD(String, theaterId, "theater_id");
    DTO_FIELD(String, startTime, "start_time");
    DTO_FIELD(Int32,  seatCapacity, "seat_capacity");

    /** Free seats at the moment the response was built. Advisory only. */
    DTO_FIELD(Int32,  availableSeats, "available_seats");
};

/** @brief The seat list for one screening. */
class SeatsDto : public oatpp::DTO {
    DTO_INIT(SeatsDto, DTO)

    DTO_FIELD(String, showtimeId, "showtime_id");
    DTO_FIELD(Int32,  seatCapacity, "seat_capacity");
    DTO_FIELD(Int32,  seatsPerRow, "seats_per_row");

    /** Free seat labels in row order, e.g. ["a1","a2","a3"]. */
    DTO_FIELD(Vector<String>, availableSeats, "available_seats");

    /** Seat labels already taken. */
    DTO_FIELD(Vector<String>, bookedSeats, "booked_seats");
};

/** @brief A confirmed reservation. */
class BookingDto : public oatpp::DTO {
    DTO_INIT(BookingDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, showtimeId, "showtime_id");
    DTO_FIELD(Vector<String>, seats);
    DTO_FIELD(String, customerName, "customer_name");
    DTO_FIELD(Int64,  createdAt, "created_at");
};

/** @brief Liveness/readiness payload for GET /api/v1/health. */
class HealthDto : public oatpp::DTO {
    DTO_INIT(HealthDto, DTO)

    DTO_FIELD(String, status);
    DTO_FIELD(String, version);
    DTO_FIELD(Int32,  movies);
    DTO_FIELD(Int32,  theaters);
    DTO_FIELD(Int32,  showtimes);
    DTO_FIELD(Int32,  bookings);
};

/** @brief Generic acknowledgement for operations with nothing to return. */
class AckDto : public oatpp::DTO {
    DTO_INIT(AckDto, DTO)

    DTO_FIELD(String, status);
    DTO_FIELD(String, detail);
};

/* =========================================================================
 * Write models (request bodies)
 * ========================================================================= */

/**
 * @brief Body of POST /api/v1/showtimes/{id}/bookings.
 *
 * `seats` is required and must hold at least one label. The service rejects
 * duplicates rather than silently collapsing them - see Catalog::bookSeats.
 */
class BookingRequestDto : public oatpp::DTO {
    DTO_INIT(BookingRequestDto, DTO)

    DTO_FIELD(Vector<String>, seats);
    DTO_FIELD(String, customerName, "customer_name");
};

/**
 * @brief Body for creating or updating a movie.
 *
 * `id` is optional on create (one is generated) and ignored on update.
 */
class MovieRequestDto : public oatpp::DTO {
    DTO_INIT(MovieRequestDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, title);
    DTO_FIELD(Int32,  durationMinutes, "duration_minutes");
    DTO_FIELD(String, language);
    DTO_FIELD(String, genre);
};

/**
 * @brief Body for creating or updating a theater.
 *
 * Omitting `seat_capacity` (or sending 0) means "use the configured default",
 * which is how the required default of 20 is reached through the API.
 */
class TheaterRequestDto : public oatpp::DTO {
    DTO_INIT(TheaterRequestDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, name);
    DTO_FIELD(String, city);
    DTO_FIELD(Int32,  seatCapacity, "seat_capacity");
    DTO_FIELD(Int32,  seatsPerRow, "seats_per_row");
};

/** @brief Body for appending or moving a single time slot. */
class ShowtimeRequestDto : public oatpp::DTO {
    DTO_INIT(ShowtimeRequestDto, DTO)

    /** Start time as "HH:MM", e.g. "14:00". Required. */
    DTO_FIELD(String, startTime, "start_time");
};

#include OATPP_CODEGEN_END(DTO)

} // namespace rest
} // namespace moviebackend

#endif // MOVIEBACKEND_DTOS_HPP
