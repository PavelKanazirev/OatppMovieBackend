/**
 * @file DtoMapper.hpp
 * @brief Translation between domain types and wire DTOs.
 *
 * This is the *only* file allowed to know about both sides. Keeping the
 * conversions here rather than inline in the controllers means:
 *   * a controller method reads as "call the service, map, respond";
 *   * a field renamed on the wire is a change in one file;
 *   * the conversions are ordinary functions, so they are easy to follow.
 *
 * Every function is `inline` and free - there is no state to hold.
 */
#ifndef MOVIEBACKEND_DTOMAPPER_HPP
#define MOVIEBACKEND_DTOMAPPER_HPP

#include "moviebackend/BookingService.hpp"
#include "moviebackend/Domain.hpp"
#include "moviebackend/Errors.hpp"
#include "moviebackend/Schedule.hpp"
#include "rest/dto/Dtos.hpp"

#include <string>
#include <vector>

namespace moviebackend {
namespace rest {

/* =========================================================================
 * Domain -> DTO  (outbound across the IPC boundary)
 * ========================================================================= */

/** @brief Convert a list of seat labels to an oatpp string vector. */
inline oatpp::Vector<oatpp::String> toDto(const std::vector<SeatLabel>& labels)
{
    oatpp::Vector<oatpp::String> result = oatpp::Vector<oatpp::String>::createShared();
    for (std::size_t i = 0; i < labels.size(); ++i) {
        result->push_back(labels[i]);
    }
    return result;
}

/** @brief Convert a Movie to its wire representation. */
inline oatpp::Object<MovieDto> toDto(const Movie& movie)
{
    oatpp::Object<MovieDto> dto = MovieDto::createShared();
    dto->id = movie.id;
    dto->title = movie.title;
    dto->durationMinutes = movie.durationMinutes;
    dto->language = movie.language;
    dto->genre = movie.genre;
    return dto;
}

/** @brief Convert a Theater to its wire representation. */
inline oatpp::Object<TheaterDto> toDto(const Theater& theater)
{
    oatpp::Object<TheaterDto> dto = TheaterDto::createShared();
    dto->id = theater.id;
    dto->name = theater.name;
    dto->city = theater.city;
    dto->seatCapacity = theater.seatCapacity;
    dto->seatsPerRow = theater.seatsPerRow;
    return dto;
}

/**
 * @brief Convert a Showtime, formatting the start time for humans.
 *
 * availableSeats is left unset (JSON null) because a bare Showtime does not
 * carry it; the overload below fills it in.
 */
inline oatpp::Object<ShowtimeDto> toDto(const Showtime& showtime)
{
    oatpp::Object<ShowtimeDto> dto = ShowtimeDto::createShared();
    dto->id = showtime.id;
    dto->movieId = showtime.movieId;
    dto->theaterId = showtime.theaterId;
    dto->startTime = schedule::formatTimeOfDay(showtime.startTime);
    dto->seatCapacity = showtime.seatCapacity;
    return dto;
}

/** @brief Convert a showtime together with its availability counts. */
inline oatpp::Object<ShowtimeDto> toDto(const ShowtimeAvailability& availability)
{
    oatpp::Object<ShowtimeDto> dto = toDto(availability.showtime);
    dto->availableSeats = availability.availableSeats;
    dto->seatCapacity = availability.totalSeats;
    return dto;
}

/** @brief Convert a Booking to its wire representation. */
inline oatpp::Object<BookingDto> toDto(const Booking& booking)
{
    oatpp::Object<BookingDto> dto = BookingDto::createShared();
    dto->id = booking.id;
    dto->showtimeId = booking.showtimeId;
    dto->seats = toDto(booking.seats);
    dto->customerName = booking.customerName;
    dto->createdAt = booking.createdAtEpochSeconds;
    return dto;
}

/**
 * @brief Convert a vector of domain values to a vector of DTOs.
 *
 * Modern C++ note: this is a function template so that one definition covers
 * movies, theaters and showtimes. It relies on the `toDto` overloads above
 * being found by overload resolution on the element type - the only piece of
 * generic programming in the codebase, and it replaces three near-identical
 * loops.
 *
 * @tparam DtoType    the wire type to produce
 * @tparam DomainType the domain type to read
 */
template <typename DtoType, typename DomainType>
inline oatpp::Vector<oatpp::Object<DtoType> > toDtoList(
    const std::vector<DomainType>& values)
{
    oatpp::Vector<oatpp::Object<DtoType> > result =
        oatpp::Vector<oatpp::Object<DtoType> >::createShared();

    for (std::size_t i = 0; i < values.size(); ++i) {
        result->push_back(toDto(values[i]));
    }
    return result;
}

/* =========================================================================
 * DTO -> domain  (inbound across the IPC boundary)
 *
 * These are where untrusted input is validated. Anything that reaches the
 * service layer from here has already been checked for presence and shape;
 * the service still checks *meaning* (does this movie exist, is this seat
 * free), because only it can.
 * ========================================================================= */

/**
 * @brief Read a required oatpp string.
 * @param value the field
 * @param fieldName used in the error message
 * @throws ServiceError InvalidArgument if the field is null or empty.
 */
inline std::string requireString(const oatpp::String& value,
                                 const std::string& fieldName)
{
    if (value.get() == nullptr || value->empty()) {
        throw invalidArgument("field '" + fieldName + "' is required");
    }
    return *value;
}

/** @brief Read an optional oatpp string, defaulting to empty. */
inline std::string optionalString(const oatpp::String& value)
{
    if (value.get() == nullptr) {
        return std::string();
    }
    return *value;
}

/** @brief Read an optional oatpp int, defaulting to @p fallback. */
inline int optionalInt(const oatpp::Int32& value, int fallback)
{
    if (value.get() == nullptr) {
        return fallback;
    }
    return *value;
}

/**
 * @brief Extract the seat list from a booking request body.
 * @throws ServiceError InvalidArgument if the body or the list is missing or
 *         empty, or if any entry is null.
 */
inline std::vector<SeatLabel> toSeatList(
    const oatpp::Object<BookingRequestDto>& body)
{
    if (body.get() == nullptr) {
        throw invalidArgument("a JSON request body is required");
    }
    if (body->seats.get() == nullptr || body->seats->empty()) {
        throw invalidArgument("field 'seats' must list at least one seat");
    }

    std::vector<SeatLabel> seats;
    seats.reserve(body->seats->size());

    for (std::size_t i = 0; i < body->seats->size(); ++i) {
        const oatpp::String& seat = (*body->seats)[i];
        if (seat.get() == nullptr || seat->empty()) {
            throw invalidArgument("field 'seats' contains an empty entry");
        }
        seats.push_back(*seat);
    }
    return seats;
}

/**
 * @brief Build a Movie from a request body.
 * @throws ServiceError InvalidArgument if required fields are missing.
 */
inline Movie toMovie(const oatpp::Object<MovieRequestDto>& body)
{
    if (body.get() == nullptr) {
        throw invalidArgument("a JSON request body is required");
    }

    Movie movie;
    movie.id = optionalString(body->id);
    movie.title = requireString(body->title, "title");
    movie.durationMinutes = optionalInt(body->durationMinutes, 0);
    movie.language = optionalString(body->language);
    movie.genre = optionalString(body->genre);

    if (movie.durationMinutes <= 0) {
        throw invalidArgument("field 'duration_minutes' must be a positive integer");
    }
    return movie;
}

/**
 * @brief Build a Theater from a request body.
 *
 * A missing capacity becomes 0, which the catalog reads as "apply the
 * configured default".
 */
inline Theater toTheater(const oatpp::Object<TheaterRequestDto>& body)
{
    if (body.get() == nullptr) {
        throw invalidArgument("a JSON request body is required");
    }

    Theater theater;
    theater.id = optionalString(body->id);
    theater.name = requireString(body->name, "name");
    theater.city = optionalString(body->city);
    theater.seatCapacity = optionalInt(body->seatCapacity, 0);
    theater.seatsPerRow = optionalInt(body->seatsPerRow, 0);

    if (theater.seatCapacity < 0 || theater.seatsPerRow < 0) {
        throw invalidArgument("seat capacity and seats per row must not be negative");
    }
    return theater;
}

/**
 * @brief Read the start time out of a showtime request body.
 * @throws ServiceError InvalidArgument if it is missing or not "HH:MM".
 */
inline MinutesSinceMidnight toStartTime(
    const oatpp::Object<ShowtimeRequestDto>& body)
{
    if (body.get() == nullptr) {
        throw invalidArgument("a JSON request body is required");
    }
    // parseTimeOfDay does the format checking and throws with a useful
    // message, so there is nothing to add here.
    return schedule::parseTimeOfDay(requireString(body->startTime, "start_time"));
}

} // namespace rest
} // namespace moviebackend

#endif // MOVIEBACKEND_DTOMAPPER_HPP
