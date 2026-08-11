/**
 * @file AdminController.hpp
 * @brief The privileged REST API. **Every method here is an IPC entry point.**
 *
 * =========================================================================
 *  IPC BOUNDARY - the administrative surface
 * =========================================================================
 *
 * Same four-step shape as PublicController (parse -> convert -> call service
 * -> respond), and the same reliance on RestErrorHandler for failures. What
 * differs is the audience and the fact that every route here mutates state.
 *
 * All routes live under `/api/v1/admin/`, which is the *entire* mechanism
 * separating the special user from an end user. The exercise explicitly
 * defers real authentication, so this is a documented gap, not an oversight:
 * anyone who can reach the port can call these. Adding an auth handler is a
 * change to this one class plus the server wiring - see the README.
 *
 * Routes, grouped by the requirement they satisfy:
 *
 *   theaters (append / modify / remove)
 *     GET    /api/v1/admin/theaters
 *     POST   /api/v1/admin/theaters
 *     PUT    /api/v1/admin/theaters/{theaterId}
 *     DELETE /api/v1/admin/theaters/{theaterId}
 *
 *   movies (append / modify / remove)
 *     GET    /api/v1/admin/movies
 *     POST   /api/v1/admin/movies
 *     PUT    /api/v1/admin/movies/{movieId}
 *     DELETE /api/v1/admin/movies/{movieId}
 *
 *   timelines (default schedule, and append / modify / remove of one slot)
 *     GET    /api/v1/admin/showtimes
 *     GET    /api/v1/admin/movies/{movieId}/theaters/{theaterId}/showtimes
 *     POST   /api/v1/admin/movies/{movieId}/theaters/{theaterId}/showtimes
 *     POST   /api/v1/admin/movies/{movieId}/theaters/{theaterId}/default-schedule
 *     PUT    /api/v1/admin/showtimes/{showtimeId}
 *     DELETE /api/v1/admin/showtimes/{showtimeId}
 *
 *   catalog file control
 *     POST   /api/v1/admin/catalog/save
 *     POST   /api/v1/admin/catalog/reload
 */
#ifndef MOVIEBACKEND_ADMINCONTROLLER_HPP
#define MOVIEBACKEND_ADMINCONTROLLER_HPP

#include "moviebackend/AdminService.hpp"
#include "moviebackend/Logging.hpp"
#include "rest/DtoMapper.hpp"
#include "rest/dto/Dtos.hpp"

#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include <memory>

namespace moviebackend {
namespace rest {

#include OATPP_CODEGEN_BEGIN(ApiController)

/**
 * @brief REST controller for the special/administrative user.
 */
class AdminController : public oatpp::web::server::api::ApiController {
public:
    /**
     * @param objectMapper JSON mapper shared with the rest of the stack
     * @param adminService the use cases; must outlive this controller
     */
    AdminController(const std::shared_ptr<ObjectMapper>& objectMapper,
                    AdminService& adminService)
        : oatpp::web::server::api::ApiController(objectMapper)
        , m_adminService(adminService)
    {
    }

    /* =====================================================================
     * Theaters - append, modify, remove
     * ===================================================================== */

    /** IPC ENTRY POINT - GET /api/v1/admin/theaters */
    ENDPOINT("GET", "/api/v1/admin/theaters", adminListTheaters)
    {
        return createDtoResponse(Status::CODE_200,
                                 toDtoList<TheaterDto>(m_adminService.listTheaters()));
    }

    /**
     * IPC ENTRY POINT - POST /api/v1/admin/theaters
     *
     * Body: `{ "name": "Hall 3", "city": "Munich", "seat_capacity": 30 }`
     *
     * Omitting `seat_capacity` gives the configured default of 20. Omitting
     * `id` generates one.
     */
    ENDPOINT("POST", "/api/v1/admin/theaters", adminAddTheater,
             BODY_DTO(Object<TheaterRequestDto>, body))
    {
        const Theater created = m_adminService.addTheater(toTheater(body));
        MB_LOG_INFO("REST admin: theater {} created", created.id);
        return createDtoResponse(Status::CODE_201, toDto(created));
    }

    /**
     * IPC ENTRY POINT - PUT /api/v1/admin/theaters/{theaterId}
     *
     * Changing the seating layout re-lays out every screening in this hall.
     * A shrink that would drop an already-booked seat is refused with 409 and
     * nothing changes.
     */
    ENDPOINT("PUT", "/api/v1/admin/theaters/{theaterId}", adminUpdateTheater,
             PATH(String, theaterId),
             BODY_DTO(Object<TheaterRequestDto>, body))
    {
        const Theater updated = m_adminService.updateTheater(
            requireString(theaterId, "theaterId"), toTheater(body));

        return createDtoResponse(Status::CODE_200, toDto(updated));
    }

    /**
     * IPC ENTRY POINT - DELETE /api/v1/admin/theaters/{theaterId}
     *
     * Destructive: removes the theater's showtimes and their bookings too.
     */
    ENDPOINT("DELETE", "/api/v1/admin/theaters/{theaterId}", adminRemoveTheater,
             PATH(String, theaterId))
    {
        const std::string id = requireString(theaterId, "theaterId");
        m_adminService.removeTheater(id);
        return acknowledge("removed",
                           "theater " + id + ", its showtimes and their bookings");
    }

    /* =====================================================================
     * Movies - append, modify, remove
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - GET /api/v1/admin/movies
     *
     * Unlike the public listing, this includes movies with no showtimes -
     * the administrator needs to see a film in order to schedule it.
     */
    ENDPOINT("GET", "/api/v1/admin/movies", adminListMovies)
    {
        return createDtoResponse(Status::CODE_200,
                                 toDtoList<MovieDto>(m_adminService.listMovies()));
    }

    /**
     * IPC ENTRY POINT - POST /api/v1/admin/movies
     *
     * Body: `{ "title": "Dune", "duration_minutes": 155, "genre": "Sci-Fi" }`
     */
    ENDPOINT("POST", "/api/v1/admin/movies", adminAddMovie,
             BODY_DTO(Object<MovieRequestDto>, body))
    {
        const Movie created = m_adminService.addMovie(toMovie(body));
        MB_LOG_INFO("REST admin: movie {} created", created.id);
        return createDtoResponse(Status::CODE_201, toDto(created));
    }

    /**
     * IPC ENTRY POINT - PUT /api/v1/admin/movies/{movieId}
     *
     * Changing the duration does not reshuffle existing screenings - that
     * would move showings people have already booked. Regenerate the timeline
     * explicitly if that is what you want.
     */
    ENDPOINT("PUT", "/api/v1/admin/movies/{movieId}", adminUpdateMovie,
             PATH(String, movieId),
             BODY_DTO(Object<MovieRequestDto>, body))
    {
        const Movie updated = m_adminService.updateMovie(
            requireString(movieId, "movieId"), toMovie(body));

        return createDtoResponse(Status::CODE_200, toDto(updated));
    }

    /**
     * IPC ENTRY POINT - DELETE /api/v1/admin/movies/{movieId}
     *
     * Destructive: removes the movie's showtimes and their bookings too.
     */
    ENDPOINT("DELETE", "/api/v1/admin/movies/{movieId}", adminRemoveMovie,
             PATH(String, movieId))
    {
        const std::string id = requireString(movieId, "movieId");
        m_adminService.removeMovie(id);
        return acknowledge("removed",
                           "movie " + id + ", its showtimes and their bookings");
    }

    /* =====================================================================
     * Timelines
     * ===================================================================== */

    /** IPC ENTRY POINT - GET /api/v1/admin/showtimes */
    ENDPOINT("GET", "/api/v1/admin/showtimes", adminListAllShowtimes)
    {
        return createDtoResponse(
            Status::CODE_200,
            toDtoList<ShowtimeDto>(m_adminService.listAllShowtimes()));
    }

    /**
     * IPC ENTRY POINT -
     * GET /api/v1/admin/movies/{movieId}/theaters/{theaterId}/showtimes
     */
    ENDPOINT("GET", "/api/v1/admin/movies/{movieId}/theaters/{theaterId}/showtimes",
             adminListShowtimes,
             PATH(String, movieId), PATH(String, theaterId))
    {
        const std::vector<Showtime> showtimes = m_adminService.listShowtimes(
            requireString(movieId, "movieId"),
            requireString(theaterId, "theaterId"));

        return createDtoResponse(Status::CODE_200,
                                 toDtoList<ShowtimeDto>(showtimes));
    }

    /**
     * IPC ENTRY POINT -
     * POST /api/v1/admin/movies/{movieId}/theaters/{theaterId}/default-schedule
     *
     * Generates the default timeline required by the exercise: 09:00, then
     * each following whole hour after the previous screening ends, up to
     * 21:00.
     *
     * **Destructive**: any existing screenings of this movie in this theater,
     * and their bookings, are replaced.
     *
     * A generated slot that clashes with a *different* film already in the
     * hall is skipped rather than failing the whole call, so the response is
     * the authoritative list of what was actually created.
     */
    ENDPOINT("POST",
             "/api/v1/admin/movies/{movieId}/theaters/{theaterId}/default-schedule",
             adminApplyDefaultSchedule,
             PATH(String, movieId), PATH(String, theaterId))
    {
        const std::vector<Showtime> created = m_adminService.applyDefaultSchedule(
            requireString(movieId, "movieId"),
            requireString(theaterId, "theaterId"));

        MB_LOG_INFO("REST admin: default schedule created {} showtimes", created.size());

        return createDtoResponse(Status::CODE_201,
                                 toDtoList<ShowtimeDto>(created));
    }

    /**
     * IPC ENTRY POINT -
     * POST /api/v1/admin/movies/{movieId}/theaters/{theaterId}/showtimes
     *
     * Appends one time slot. Body: `{ "start_time": "14:00" }`.
     *
     * The time must lie inside the configured screening day (09:00..21:00 by
     * default) and the hall must be free at that minute.
     */
    ENDPOINT("POST", "/api/v1/admin/movies/{movieId}/theaters/{theaterId}/showtimes",
             adminAddShowtime,
             PATH(String, movieId), PATH(String, theaterId),
             BODY_DTO(Object<ShowtimeRequestDto>, body))
    {
        const Showtime created = m_adminService.addShowtime(
            requireString(movieId, "movieId"),
            requireString(theaterId, "theaterId"),
            toStartTime(body));

        return createDtoResponse(Status::CODE_201, toDto(created));
    }

    /**
     * IPC ENTRY POINT - PUT /api/v1/admin/showtimes/{showtimeId}
     *
     * Moves a screening. Body: `{ "start_time": "16:00" }`.
     * Existing bookings are kept - the audience keeps its seats.
     */
    ENDPOINT("PUT", "/api/v1/admin/showtimes/{showtimeId}", adminUpdateShowtime,
             PATH(String, showtimeId),
             BODY_DTO(Object<ShowtimeRequestDto>, body))
    {
        const Showtime updated = m_adminService.updateShowtime(
            requireString(showtimeId, "showtimeId"), toStartTime(body));

        return createDtoResponse(Status::CODE_200, toDto(updated));
    }

    /**
     * IPC ENTRY POINT - DELETE /api/v1/admin/showtimes/{showtimeId}
     *
     * Destructive: cancels every booking against this screening.
     */
    ENDPOINT("DELETE", "/api/v1/admin/showtimes/{showtimeId}", adminRemoveShowtime,
             PATH(String, showtimeId))
    {
        const std::string id = requireString(showtimeId, "showtimeId");
        m_adminService.removeShowtime(id);
        return acknowledge("removed", "showtime " + id + " and its bookings");
    }

    /* =====================================================================
     * Catalog file control
     * ===================================================================== */

    /**
     * IPC ENTRY POINT - POST /api/v1/admin/catalog/save
     *
     * Forces a synchronous write. Autosave normally handles this in the
     * background; this responds only once the bytes are on disk, so an
     * administrator can be sure a checkpoint landed.
     */
    ENDPOINT("POST", "/api/v1/admin/catalog/save", adminSaveCatalog)
    {
        m_adminService.saveCatalog();
        return acknowledge("saved", "the catalog was written to its JSON file");
    }

    /**
     * IPC ENTRY POINT - POST /api/v1/admin/catalog/reload
     *
     * Re-reads the catalog file, for when the administrator edited it by hand.
     *
     * **Destructive**: any booking made since the last save is lost. If the
     * file is invalid the request fails with 400 and the live catalog is left
     * exactly as it was.
     */
    ENDPOINT("POST", "/api/v1/admin/catalog/reload", adminReloadCatalog)
    {
        m_adminService.reloadCatalog();
        return acknowledge("reloaded", "the catalog was re-read from its JSON file");
    }

private:
    /**
     * @brief Build the standard acknowledgement response.
     *
     * Exists so the several "deleted"/"saved" endpoints above do not repeat
     * four lines of DTO assembly each.
     */
    std::shared_ptr<OutgoingResponse> acknowledge(const std::string& status,
                                                  const std::string& detail)
    {
        oatpp::Object<AckDto> body = AckDto::createShared();
        body->status = status;
        body->detail = detail;
        return createDtoResponse(Status::CODE_200, body);
    }

    /** Non-owning: the service is created in main() and outlives the server. */
    AdminService& m_adminService;
};

#include OATPP_CODEGEN_END(ApiController)

} // namespace rest
} // namespace moviebackend

#endif // MOVIEBACKEND_ADMINCONTROLLER_HPP
