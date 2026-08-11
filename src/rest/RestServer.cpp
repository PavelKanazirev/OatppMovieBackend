/**
 * @file RestServer.cpp
 * @brief Wiring and lifecycle of the oatpp HTTP stack.
 *
 * ## Why the components are wired by hand
 *
 * oatpp's documented style uses the OATPP_CREATE_COMPONENT macro, which
 * registers each component in a global registry and requires a lambda per
 * component. That is convenient in a large application with many optional
 * pieces. Here it would mean five lambdas and a hidden global, in exchange for
 * nothing - this service has exactly one server with a fixed set of parts.
 *
 * Constructing them explicitly in the constructor below makes the dependency
 * order visible (mapper -> router -> controllers -> handler -> provider ->
 * server) and keeps the codebase free of lambdas it does not need.
 *
 * ## The shared_ptr exception, restated
 *
 * Every handle in Impl is a std::shared_ptr because oatpp's factories return
 * them and its APIs take them. This is the one place in the project where
 * that is true, and it is a constraint of the framework rather than a design
 * choice. Nothing outside this file sees a shared_ptr.
 */
#include "moviebackend/RestServer.hpp"

#include "moviebackend/Errors.hpp"
#include "moviebackend/Logging.hpp"
#include "rest/AdminController.hpp"
#include "rest/ErrorHandler.hpp"
#include "rest/PublicController.hpp"

#include "oatpp/network/Server.hpp"
#include "oatpp/network/ConnectionProvider.hpp"
#include "oatpp/network/tcp/server/ConnectionProvider.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/HttpRouter.hpp"

#include <atomic>
#include <exception>
#include <string>

namespace moviebackend {

/**
 * @brief Everything oatpp needs, hidden from the public header (PIMPL).
 */
struct RestServer::Impl {
    std::shared_ptr<oatpp::parser::json::mapping::ObjectMapper> objectMapper;
    std::shared_ptr<oatpp::web::server::HttpRouter>             router;
    std::shared_ptr<rest::RestErrorHandler>                     errorHandler;
    std::shared_ptr<rest::PublicController>                     publicController;
    std::shared_ptr<rest::AdminController>                      adminController;
    std::shared_ptr<oatpp::web::server::HttpConnectionHandler>  connectionHandler;
    std::shared_ptr<oatpp::network::tcp::server::ConnectionProvider> connectionProvider;
    std::shared_ptr<oatpp::network::Server>                     server;

    /**
     * Guards against a stop request that arrives *before* start() has begun
     * listening, and makes requestStop() idempotent.
     */
    std::atomic<bool> stopRequested{false};
};

RestServer::RestServer(const ServerConfig& serverConfig,
                       BookingService& bookingService,
                       AdminService& adminService)
    : m_impl(new Impl())
{
    // oatpp keeps process-wide state (allocation counters, the component
    // registry). It must be initialised before any oatpp object exists and
    // destroyed after the last one - main() owns that lifecycle, because a
    // test may construct more than one RestServer in a process.

    /* ---- 1. JSON mapper -------------------------------------------------
     * Shared by the controllers and the error handler, so success bodies and
     * error bodies are formatted identically.
     */
    m_impl->objectMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

    // Do not emit fields that were never assigned. Without this, every
    // response would carry a pile of nulls (a ShowtimeDto with no
    // availability figure, for instance).
    m_impl->objectMapper->getSerializer()->getConfig()->includeNullFields = false;

    /* ---- 2. Router and error handler ------------------------------------ */
    m_impl->router = oatpp::web::server::HttpRouter::createShared();
    m_impl->errorHandler = std::make_shared<rest::RestErrorHandler>(m_impl->objectMapper);

    /* ---- 3. Controllers -------------------------------------------------
     * addController() walks the ENDPOINT macros each controller declared and
     * registers every route. The two controllers are separate objects on
     * separate path prefixes, which is the whole mechanism separating the
     * end-user API from the administrative one.
     */
    m_impl->publicController =
        std::make_shared<rest::PublicController>(m_impl->objectMapper, bookingService);
    m_impl->adminController =
        std::make_shared<rest::AdminController>(m_impl->objectMapper, adminService);

    m_impl->router->addController(m_impl->publicController);
    m_impl->router->addController(m_impl->adminController);

    /* ---- 4. Connection handler ------------------------------------------
     * This is what spawns a thread per connection, which is why every
     * controller method must be safe to run concurrently.
     */
    m_impl->connectionHandler =
        oatpp::web::server::HttpConnectionHandler::createShared(m_impl->router);
    m_impl->connectionHandler->setErrorHandler(m_impl->errorHandler);

    /* ---- 5. TCP listener ------------------------------------------------
     * Constructing this binds the socket, so a port clash is reported here
     * rather than at start().
     *
     * A configured port of 0 asks the OS for a free one. See boundPort()
     * for how the resulting port is recovered - it is not where you would
     * expect it to be.
     */
    const oatpp::network::Address address(serverConfig.host,
                                          serverConfig.port,
                                          oatpp::network::Address::IP_4);
    try {
        m_impl->connectionProvider =
            oatpp::network::tcp::server::ConnectionProvider::createShared(address);
    } catch (const std::exception& error) {
        throw ServiceError(ErrorCode::Internal,
                           "cannot listen on " + serverConfig.host + ":"
                               + std::to_string(serverConfig.port) + " - "
                               + error.what());
    }

    /* ---- 6. The server itself ------------------------------------------- */
    m_impl->server = std::make_shared<oatpp::network::Server>(
        m_impl->connectionProvider, m_impl->connectionHandler);

    MB_LOG_INFO("REST server ready on {}:{}", serverConfig.host, boundPort());
}

RestServer::~RestServer()
{
    // Make sure the accept loop is not still running when the components it
    // uses are destroyed.
    requestStop();

    if (m_impl->connectionHandler != nullptr) {
        // Waits for in-flight requests to finish, so no worker thread is
        // touching a controller while it is being torn down.
        m_impl->connectionHandler->stop();
    }
}

void RestServer::start()
{
    // A stop that arrived before we got here must not be lost.
    if (m_impl->stopRequested.load()) {
        MB_LOG_WARN("REST server: start() called after a stop request - not starting");
        return;
    }

    MB_LOG_INFO("REST server: entering accept loop");

    // Blocks in accept() until requestStop() unblocks it by closing the
    // listening socket.
    m_impl->server->run();

    MB_LOG_INFO("REST server: accept loop finished");
}

void RestServer::requestStop() noexcept
{
    // exchange() makes this idempotent and safe from any thread - the
    // destructor and a signal handler may both call it.
    if (m_impl->stopRequested.exchange(true)) {
        return;
    }

    MB_LOG_INFO("REST server: stop requested");

    // The order matters. stop() flips the server's state flag, but the thread
    // is blocked inside accept() and will not notice until something wakes
    // it. Closing the listening socket is what does that.
    if (m_impl->server != nullptr) {
        m_impl->server->stop();
    }
    if (m_impl->connectionProvider != nullptr) {
        m_impl->connectionProvider->stop();
    }
}

std::uint16_t RestServer::boundPort() const noexcept
{
    if (m_impl->connectionProvider == nullptr) {
        return 0;
    }

    // Read the port from the provider's PROPERTY_PORT rather than from
    // getAddress().port.
    //
    // This looks like the long way round, and it is - but it is the only one
    // that is correct. oatpp binds the socket in the ConnectionProvider
    // constructor and, when the requested port was 0, learns the real port
    // from getsockname(). It then writes that value **only into the string
    // property**; the `Address` member it was constructed with keeps the
    // original 0. So getAddress().port would report 0 forever, and the smoke
    // tests (which rely on port 0 to avoid clashing with a running instance)
    // would never find the server.
    const oatpp::data::share::StringKeyLabel portProperty =
        m_impl->connectionProvider->getProperty(
            oatpp::network::ConnectionProvider::PROPERTY_PORT);

    if (portProperty) {
        const oatpp::String portText = portProperty.toString();
        if (portText.get() != nullptr) {
            try {
                const int parsed = std::stoi(portText->c_str());
                if (parsed > 0 && parsed <= 65535) {
                    return static_cast<std::uint16_t>(parsed);
                }
            } catch (const std::exception&) {
                // Fall through to the address below - a port we cannot parse
                // is not worth failing a getter over.
            }
        }
    }

    return m_impl->connectionProvider->getAddress().port;
}

} // namespace moviebackend
