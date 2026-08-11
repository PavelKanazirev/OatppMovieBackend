/**
 * @file ErrorHandler.hpp
 * @brief The single place where a failure becomes an HTTP status code.
 */
#ifndef MOVIEBACKEND_RESTERRORHANDLER_HPP
#define MOVIEBACKEND_RESTERRORHANDLER_HPP

#include "moviebackend/Errors.hpp"

#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/handler/ErrorHandler.hpp"

#include <memory>

namespace moviebackend {
namespace rest {

/**
 * @brief Turns any exception escaping a controller into a JSON error response.
 *
 * Design pattern: this is oatpp's **Strategy** hook for error rendering - the
 * framework calls it instead of producing its own default page.
 *
 * ## Why this class earns its place
 *
 * Without it, every controller method would need its own try/catch and its
 * own idea of which status code to use. The result would be an API where a
 * missing movie is a 404 on one route and a 500 on another. Here the mapping
 * is written once:
 *
 *   | ErrorCode           | HTTP | Meaning                                   |
 *   |---------------------|------|-------------------------------------------|
 *   | NotFound            | 404  | no such movie/theater/showtime/seat       |
 *   | InvalidArgument     | 400  | malformed request                         |
 *   | Conflict            | 409  | clashes with current state                |
 *   | SeatUnavailable     | 409  | somebody else took the seat               |
 *   | PersistenceFailure  | 500  | could not read or write the catalog file  |
 *   | Internal            | 500  | anything unanticipated                    |
 *
 * SeatUnavailable is deliberately given its own error *code* in the body even
 * though it shares 409 with Conflict, so that a client can distinguish "retry
 * with different seats" from "your request was inconsistent".
 *
 * ## Threading
 *
 * oatpp calls this from whichever thread was serving the failed request, so
 * it must be re-entrant. It is: the only state is the object mapper, which is
 * used read-only.
 */
class RestErrorHandler : public oatpp::web::server::handler::ErrorHandler {
public:
    /**
     * @param objectMapper used to serialise the ErrorDto body; shared with
     *        the controllers so that error bodies and success bodies are
     *        formatted identically
     */
    explicit RestErrorHandler(
        const std::shared_ptr<oatpp::data::mapping::ObjectMapper>& objectMapper);

    /**
     * @brief Entry point for an exception thrown out of a controller.
     *
     * Unwraps the exception, maps a ServiceError to its status code, and
     * treats anything else as a 500.
     *
     * @param error the exception that escaped
     * @return the response to send
     */
    std::shared_ptr<oatpp::web::protocol::http::outgoing::Response> handleError(
        const std::exception_ptr& error) override;

    /**
     * @brief Entry point for errors oatpp itself raises (bad JSON body,
     *        unroutable path, ...).
     *
     * @param status  the status oatpp chose
     * @param message its explanation
     * @param headers headers to preserve on the response
     */
    std::shared_ptr<oatpp::web::protocol::http::outgoing::Response> handleError(
        const oatpp::web::protocol::http::Status& status,
        const oatpp::String& message,
        const Headers& headers) override;

private:
    /** @brief Map a domain error code to its HTTP status. */
    static oatpp::web::protocol::http::Status toHttpStatus(ErrorCode code);

    /** @brief Build the JSON error body and wrap it in a response. */
    std::shared_ptr<oatpp::web::protocol::http::outgoing::Response> makeResponse(
        const oatpp::web::protocol::http::Status& status,
        const std::string& code,
        const std::string& message,
        const Headers& headers);

    std::shared_ptr<oatpp::data::mapping::ObjectMapper> m_objectMapper;
};

} // namespace rest
} // namespace moviebackend

#endif // MOVIEBACKEND_RESTERRORHANDLER_HPP
