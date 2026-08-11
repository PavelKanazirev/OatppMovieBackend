/**
 * @file Errors.hpp
 * @brief Error taxonomy shared by every layer of the backend.
 *
 * ## Why exceptions and not a Result/expected type
 *
 * C++23 offers `std::expected`, and it would be a defensible choice here.
 * We deliberately use a plain exception type instead:
 *
 *   * The failure cases in this service are genuinely exceptional - a missing
 *     movie id, a seat that somebody else just took. They are not part of the
 *     normal control flow of the caller.
 *   * The REST layer needs exactly one place that turns a failure into an
 *     HTTP status code. An exception travelling up to a single error handler
 *     expresses that far more directly than threading `std::expected` through
 *     every controller method.
 *   * It keeps the call sites readable, which is the stated priority of this
 *     project.
 *
 * The one rule everybody follows: **only ServiceError crosses a layer
 * boundary.** Lower layers never let a `std::out_of_range` from a container
 * escape; they translate it.
 */
#ifndef MOVIEBACKEND_ERRORS_HPP
#define MOVIEBACKEND_ERRORS_HPP

#include <stdexcept>
#include <string>

namespace moviebackend {

/**
 * @brief Machine-readable classification of a failure.
 *
 * The REST error handler maps each value to one HTTP status code, so this
 * enum is effectively the public error contract of the service.
 */
enum class ErrorCode {
    /** The caller asked for an entity that does not exist. -> HTTP 404 */
    NotFound,

    /** The request was structurally wrong (bad time format, empty seat list,
     *  negative capacity, ...). -> HTTP 400 */
    InvalidArgument,

    /** The request was well formed but conflicts with the current state
     *  (duplicate id, a showtime already occupies that slot). -> HTTP 409 */
    Conflict,

    /** At least one requested seat is already booked. This is the
     *  over-booking guard firing and it deserves its own code so that a
     *  client can retry intelligently. -> HTTP 409 */
    SeatUnavailable,

    /** Reading or writing the catalog file failed. -> HTTP 500 */
    PersistenceFailure,

    /** Anything we did not anticipate. -> HTTP 500 */
    Internal
};

/**
 * @brief Human readable name of an ErrorCode, used in JSON error payloads.
 * @param code the code to describe
 * @return a stable, machine-friendly string such as "SEAT_UNAVAILABLE"
 */
const char* toString(ErrorCode code) noexcept;

/**
 * @brief The single exception type that crosses layer boundaries.
 *
 * Design pattern: none in particular - this is a plain domain exception. It
 * carries a code (for the HTTP mapping) plus a message (for the human reading
 * the log or the JSON body).
 */
class ServiceError : public std::runtime_error {
public:
    /**
     * @param code    classification used by the REST error handler
     * @param message human readable detail, safe to show to an API client
     */
    ServiceError(ErrorCode code, const std::string& message)
        : std::runtime_error(message)
        , m_code(code)
    {
    }

    /** @return the classification of this failure. */
    ErrorCode code() const noexcept { return m_code; }

private:
    ErrorCode m_code;
};

/* -------------------------------------------------------------------------
 * Small named constructors.
 *
 * These exist purely so that throw sites read as prose:
 *     throw notFound("movie", movieId);
 * rather than repeating the enum and the string formatting everywhere.
 * ------------------------------------------------------------------------- */

/** @brief Build a NotFound error for @p entity with identifier @p id. */
ServiceError notFound(const std::string& entity, const std::string& id);

/** @brief Build an InvalidArgument error with the given @p message. */
ServiceError invalidArgument(const std::string& message);

/** @brief Build a Conflict error with the given @p message. */
ServiceError conflict(const std::string& message);

} // namespace moviebackend

#endif // MOVIEBACKEND_ERRORS_HPP
