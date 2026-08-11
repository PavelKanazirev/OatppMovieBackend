/**
 * @file RestServer.hpp
 * @brief The IPC boundary: an oatpp HTTP server exposing the two REST APIs.
 *
 * =========================================================================
 *  THE IPC BOUNDARY OF THIS SERVICE
 * =========================================================================
 *
 * "IPC" in this project means HTTP/JSON over TCP. Everything on the far side
 * of this class is a separate process - the Python smoke tests, curl, a
 * browser. Everything on this side is plain in-process C++.
 *
 * There are exactly three kinds of boundary crossing, and they all live in
 * src/rest/:
 *
 *   1. **Inbound**  - oatpp parses the request line, path parameters and the
 *      JSON body into DTOs (src/rest/dto/Dtos.hpp).
 *   2. **Dispatch** - a controller method converts DTOs into domain types and
 *      calls BookingService or AdminService. These controller methods are
 *      *the* IPC entry points; each one is marked as such in its comment.
 *   3. **Outbound** - the return value is converted back to a DTO, or a
 *      thrown ServiceError is turned into a JSON error body by
 *      RestErrorHandler.
 *
 * No domain type ever appears on the wire and no DTO ever reaches the
 * catalog. That separation is why the storage format and the wire format can
 * evolve independently.
 *
 * ## Threading
 *
 * oatpp's HttpConnectionHandler runs each connection on its own thread, so
 * every controller method below can execute concurrently with every other
 * one. The controllers hold no mutable state; all shared state is behind the
 * Catalog lock.
 *
 * ## The std::shared_ptr exception
 *
 * This project uses std::unique_ptr everywhere, with one exception: oatpp's
 * public API is built entirely on std::shared_ptr (`createShared()` factories,
 * shared_ptr parameters on every handler). Storing those handles in anything
 * else is not possible without fighting the framework. The shared_ptrs are
 * therefore confined to this class and the controllers - they never appear in
 * the domain, service or persistence layers.
 */
#ifndef MOVIEBACKEND_RESTSERVER_HPP
#define MOVIEBACKEND_RESTSERVER_HPP

#include "moviebackend/AdminService.hpp"
#include "moviebackend/BookingService.hpp"
#include "moviebackend/Config.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace moviebackend {

/**
 * @brief Owns the oatpp stack and runs the HTTP server.
 *
 * Design pattern: **Facade** over oatpp's component wiring, plus **PIMPL** -
 * the oatpp types are hidden in the .cpp so that this header (and therefore
 * everything that includes it, including the tests) does not drag in the
 * whole framework.
 *
 * Typical use:
 * @code
 *   RestServer server(config.server, bookingService, adminService);
 *   server.start();          // blocks until requestStop()
 * @endcode
 */
class RestServer {
public:
    /**
     * @param serverConfig   listen address and port
     * @param bookingService end-user use cases; must outlive this server
     * @param adminService   admin use cases; must outlive this server
     */
    RestServer(const ServerConfig& serverConfig,
               BookingService& bookingService,
               AdminService& adminService);

    ~RestServer();

    RestServer(const RestServer&) = delete;
    RestServer& operator=(const RestServer&) = delete;

    /**
     * @brief Run the server. Blocks until requestStop() is called.
     *
     * @throws ServiceError Internal if the port cannot be bound.
     */
    void start();

    /**
     * @brief Ask the server to stop and return from start().
     *
     * Safe to call from any thread, including from a signal handler context
     * where it is the only thing the handler does. Idempotent.
     */
    void requestStop() noexcept;

    /**
     * @brief The port actually in use.
     *
     * Relevant when the configured port is 0, which asks the OS to pick a
     * free one - the smoke tests use that to avoid clashing with a server
     * already running on the machine.
     */
    std::uint16_t boundPort() const noexcept;

private:
    /** Hidden oatpp state; see the PIMPL note above. */
    struct Impl;

    /**
     * unique_ptr, as everywhere else in this project. The shared_ptrs that
     * oatpp insists on live *inside* Impl.
     */
    std::unique_ptr<Impl> m_impl;
};

} // namespace moviebackend

#endif // MOVIEBACKEND_RESTSERVER_HPP
