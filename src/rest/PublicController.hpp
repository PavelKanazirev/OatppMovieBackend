/**
 * @file PublicController.hpp
 * @brief The end-user REST API. **Every method here is an IPC entry point.**
 *
 * =========================================================================
 *  IPC BOUNDARY - the end-user surface
 * =========================================================================
 *
 * Each ENDPOINT below is a function that runs on an oatpp worker thread with
 * data that came off a socket. They all follow the same four steps:
 *
 *     1. take the path parameters / body that oatpp already parsed
 *     2. validate and convert them to domain types  (DtoMapper)
 *     3. call BookingService                        (the actual work)
 *     4. convert the result back to a DTO and respond
 *
 * There is no try/catch anywhere: a ServiceError thrown in step 2 or 3
 * travels up into RestErrorHandler, which owns the entire error-to-HTTP
 * mapping. That is why these methods read as straight-line code.
 *
 * The routes follow the user journey from the requirements:
 *
 *     GET  /api/v1/movies                                        view all playing movies
 *     GET  /api/v1/movies/{movieId}                              select a movie
 *     GET  /api/v1/movies/{movieId}/theaters                     theaters showing it
 *     GET  /api/v1/movies/{movieId}/theaters/{theaterId}/showtimes   its timeline there
 *     GET  /api/v1/showtimes/{showtimeId}/seats                  available seats
 *     POST /api/v1/showtimes/{showtimeId}/bookings               book seats
 *     GET  /api/v1/bookings/{bookingId}                          the receipt
 *     DELETE /api/v1/bookings/{bookingId}                        cancel
 *
 * ## Threading
 *
 * oatpp serves each connection on its own thread, so every method here can run
 * concurrently with every other one. The controller holds no mutable state -
 * only a reference to the service - so there is nothing here to protect. All
 * shared state lives behind the Catalog lock.
 */
#ifndef MOVIEBACKEND_PUBLICCONTROLLER_HPP
#define MOVIEBACKEND_PUBLICCONTROLLER_HPP

#include "moviebackend/BookingService.hpp"
#include "moviebackend/Logging.hpp"
#include "rest/DtoMapper.hpp"
#include "rest/dto/Dtos.hpp"

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include <memory>

namespace moviebackend {
namespace rest {

// Everything below may use the ENDPOINT macros until OATPP_CODEGEN_END.
#include OATPP_CODEGEN_BEGIN(ApiController)

/**
 * @brief REST controller for end users.
 */
class PublicController : public oatpp::web::server::api::ApiController {
public:
    /**
     * @param objectMapper  JSON mapper shared with the rest of the stack
     * @param bookingService the use cases; must outlive this controller
     */
    PublicController(const std::shared_ptr<ObjectMapper>& objectMapper,
                     BookingService& bookingService)
        : oatpp::web::server::api::ApiController(objectMapper)
        , m_bookingService(bookingService)
    {
    }

    /* =====================================================================
     * Health
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - GET /api/v1/health
     *
     * Used by the smoke tests to wait for the server to come up, and by the
     * memcheck script to know when it is safe to start driving traffic.
     */
    ENDPOINT("GET", "/api/v1/health", healthCheck)
    {
        oatpp::Object<HealthDto> body = HealthDto::createShared();
        body->status = "ok";
        body->version = "1.0.0";
        return createDtoResponse(Status::CODE_200, body);
    }

    /* =====================================================================
     * Step 1 - view all playing movies
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - GET /api/v1/movies
     *
     * Lists only movies that actually have a screening somewhere; a movie the
     * administrator has created but not yet scheduled is not offered here.
     */
    ENDPOINT("GET", "/api/v1/movies", listMovies)
    {
        const std::vector<Movie> movies = m_bookingService.listPlayingMovies();
        return createDtoResponse(Status::CODE_200,
                                 toDtoList<MovieDto>(movies));
    }

    /* =====================================================================
     * Step 2 - select a movie
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - GET /api/v1/movies/{movieId}
     *
     * `movieId` is taken from the path. Responds 404 if there is no such
     * movie.
     */
    ENDPOINT("GET", "/api/v1/movies/{movieId}", getMovie,
             PATH(String, movieId))
    {
        const Movie movie = m_bookingService.getMovie(requireString(movieId, "movieId"));
        return createDtoResponse(Status::CODE_200, toDto(movie));
    }

    /* =====================================================================
     * Step 3 - see all theaters showing that movie
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - GET /api/v1/movies/{movieId}/theaters
     *
     * An empty array means "this film is not on anywhere"; a 404 means "there
     * is no such film". The distinction is deliberate.
     */
    ENDPOINT("GET", "/api/v1/movies/{movieId}/theaters", listTheatersForMovie,
             PATH(String, movieId))
    {
        const std::vector<Theater> theaters =
            m_bookingService.listTheatersShowingMovie(requireString(movieId, "movieId"));

        return createDtoResponse(Status::CODE_200,
                                 toDtoList<TheaterDto>(theaters));
    }

    /* =====================================================================
     * Step 4 - select a theater, see the timeline
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - GET /api/v1/movies/{movieId}/theaters/{theaterId}/showtimes
     *
     * Each entry carries `available_seats` so a client can grey out a
     * sold-out screening without a second round trip.
     */
    ENDPOINT("GET", "/api/v1/movies/{movieId}/theaters/{theaterId}/showtimes",
             listShowtimes,
             PATH(String, movieId), PATH(String, theaterId))
    {
        const std::vector<ShowtimeAvailability> showtimes =
            m_bookingService.listShowtimes(requireString(movieId, "movieId"),
                                           requireString(theaterId, "theaterId"));

        return createDtoResponse(Status::CODE_200,
                                 toDtoList<ShowtimeDto>(showtimes));
    }

    /* =====================================================================
     * Step 5 - see available seats
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - GET /api/v1/showtimes/{showtimeId}/seats
     *
     * Returns both the free and the taken seats. The free list is what the
     * user picks from; the taken list lets a seat-map UI render the whole
     * auditorium from one response.
     *
     * The counts are a snapshot: a seat listed as free here can be taken by
     * somebody else before the booking request arrives. That is expected, and
     * it is exactly why the booking call re-checks under the lock.
     */
    ENDPOINT("GET", "/api/v1/showtimes/{showtimeId}/seats", listSeats,
             PATH(String, showtimeId))
    {
        const std::string id = requireString(showtimeId, "showtimeId");

        // getShowtime() first so an unknown id is a 404 before anything else.
        const Showtime showtime = m_bookingService.getShowtime(id);

        oatpp::Object<SeatsDto> body = SeatsDto::createShared();
        body->showtimeId = id;
        body->seatCapacity = showtime.seatCapacity;
        body->seatsPerRow = showtime.seatsPerRow;
        body->availableSeats = toDto(m_bookingService.listAvailableSeats(id));
        body->bookedSeats = toDto(m_bookingService.listBookedSeats(id));

        return createDtoResponse(Status::CODE_200, body);
    }

    /* =====================================================================
     * Step 6 - book one or more of the available seats
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - POST /api/v1/showtimes/{showtimeId}/bookings
     *
     * **This is the endpoint the concurrency requirement is about.** Many
     * clients may hit it simultaneously for the same seats; the guarantee
     * that at most one wins is enforced inside Catalog::bookSeats, not here.
     *
     * Request body:
     * @code
     *   { "seats": ["a1", "a2"], "customer_name": "Alice" }
     * @endcode
     *
     * Responses:
     *   * 201 with the booking on success
     *   * 400 if the body is missing/empty or lists a seat twice
     *   * 404 if the showtime or a seat label does not exist
     *   * 409 (SEAT_UNAVAILABLE) if any requested seat is already taken -
     *     in which case **nothing** was booked
     */
    ENDPOINT("POST", "/api/v1/showtimes/{showtimeId}/bookings", createBooking,
             PATH(String, showtimeId),
             BODY_DTO(Object<BookingRequestDto>, body))
    {
        const std::string id = requireString(showtimeId, "showtimeId");
        const std::vector<SeatLabel> seats = toSeatList(body);
        const std::string customerName =
            (body.get() == nullptr) ? std::string() : optionalString(body->customerName);

        const Booking booking = m_bookingService.bookSeats(id, seats, customerName);

        MB_LOG_INFO("REST: booking {} created for showtime {}", booking.id, id);

        // 201 Created, because a new resource now exists at
        // /api/v1/bookings/{id}.
        return createDtoResponse(Status::CODE_201, toDto(booking));
    }

    /* =====================================================================
     * Booking receipts
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - GET /api/v1/bookings/{bookingId}
     */
    ENDPOINT("GET", "/api/v1/bookings/{bookingId}", getBooking,
             PATH(String, bookingId))
    {
        const Booking booking =
            m_bookingService.getBooking(requireString(bookingId, "bookingId"));
        return createDtoResponse(Status::CODE_200, toDto(booking));
    }

    /**
     * IPC ENTRY POINT - DELETE /api/v1/bookings/{bookingId}
     *
     * Releases the seats back into the pool.
     */
    ENDPOINT("DELETE", "/api/v1/bookings/{bookingId}", cancelBooking,
             PATH(String, bookingId))
    {
        const std::string id = requireString(bookingId, "bookingId");
        m_bookingService.cancelBooking(id);

        oatpp::Object<AckDto> body = AckDto::createShared();
        body->status = "cancelled";
        body->detail = "booking " + id + " was cancelled and its seats released";

        return createDtoResponse(Status::CODE_200, body);
    }

private:
    /** Non-owning: the service is created in main() and outlives the server. */
    BookingService& m_bookingService;
};

#include OATPP_CODEGEN_END(ApiController)

} // namespace rest
} // namespace moviebackend

#endif // MOVIEBACKEND_PUBLICCONTROLLER_HPP
