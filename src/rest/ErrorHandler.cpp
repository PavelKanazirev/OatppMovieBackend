/**
 * @file ErrorHandler.cpp
 * @brief Implementation of the exception-to-HTTP mapping.
 */
#include "rest/ErrorHandler.hpp"

#include "moviebackend/Logging.hpp"
#include "rest/dto/Dtos.hpp"

#include <string>

#include "oatpp/core/parser/ParsingError.hpp"
#include "oatpp/web/protocol/http/outgoing/BufferBody.hpp"
#include "oatpp/web/protocol/http/outgoing/ResponseFactory.hpp"

namespace moviebackend {
namespace rest {

namespace http = oatpp::web::protocol::http;

RestErrorHandler::RestErrorHandler(
    const std::shared_ptr<oatpp::data::mapping::ObjectMapper>& objectMapper)
    : m_objectMapper(objectMapper)
{
}

http::Status RestErrorHandler::toHttpStatus(ErrorCode code)
{
    // No default label, so adding an ErrorCode without deciding its HTTP
    // status becomes a compiler warning rather than a silent 500.
    switch (code) {
        case ErrorCode::NotFound:           return http::Status::CODE_404;
        case ErrorCode::InvalidArgument:    return http::Status::CODE_400;
        case ErrorCode::Conflict:           return http::Status::CODE_409;
        case ErrorCode::SeatUnavailable:    return http::Status::CODE_409;
        case ErrorCode::PersistenceFailure: return http::Status::CODE_500;
        case ErrorCode::Internal:           return http::Status::CODE_500;
    }
    return http::Status::CODE_500;
}

std::shared_ptr<http::outgoing::Response> RestErrorHandler::makeResponse(
    const http::Status& status,
    const std::string& code,
    const std::string& message,
    const Headers& headers)
{
    oatpp::Object<ErrorDto> body = ErrorDto::createShared();
    body->code = code;
    body->message = message;

    const oatpp::String serialised = m_objectMapper->writeToString(body);

    std::shared_ptr<http::outgoing::Response> response =
        http::outgoing::ResponseFactory::createResponse(status, serialised);

    response->putHeaderIfNotExists("Content-Type", "application/json");

    // Preserve whatever headers oatpp wanted on the response (CORS, Connection
    // handling and so on) - dropping them would break keep-alive behaviour.
    for (const auto& header : headers.getAll()) {
        response->putHeaderIfNotExists(header.first.toString(),
                                       header.second.toString());
    }

    return response;
}

std::shared_ptr<http::outgoing::Response> RestErrorHandler::handleError(
    const std::exception_ptr& error)
{
    Headers noHeaders;

    // Rethrow-and-catch is the standard way to inspect a std::exception_ptr;
    // there is no way to query it directly.
    try {
        if (error != nullptr) {
            std::rethrow_exception(error);
        }
        // A null exception_ptr should not happen, but returning a coherent
        // 500 is better than dereferencing nothing.
        MB_LOG_ERROR("REST: error handler invoked with no exception");
        return makeResponse(http::Status::CODE_500,
                            toString(ErrorCode::Internal),
                            "internal error", noHeaders);

    } catch (const ServiceError& serviceError) {
        // The expected path: a domain failure with a known classification.
        const http::Status status = toHttpStatus(serviceError.code());

        // 4xx is the client's problem and is normal traffic, so it is logged
        // at debug. 5xx is our problem and is logged at error - which matters
        // because a release build only keeps error and above.
        if (status.code >= 500) {
            MB_LOG_ERROR("REST: {} -> HTTP {}: {}",
                         toString(serviceError.code()), status.code,
                         serviceError.what());
        } else {
            MB_LOG_DEBUG("REST: {} -> HTTP {}: {}",
                         toString(serviceError.code()), status.code,
                         serviceError.what());
        }

        return makeResponse(status, toString(serviceError.code()),
                            serviceError.what(), noHeaders);

    } catch (const oatpp::parser::ParsingError& parsingError) {
        // The client sent a body that is not valid JSON, so oatpp's
        // deserialiser threw while building the DTO.
        //
        // This branch has to come *before* the std::exception one below:
        // ParsingError derives from std::runtime_error, so without it a
        // malformed body would be reported as a 500. That would be wrong
        // twice over - it blames the server for the client's mistake, and it
        // pollutes the server-error rate that alerting keys off.
        MB_LOG_DEBUG("REST: malformed JSON body -> HTTP 400: {}",
                     parsingError.what());

        return makeResponse(http::Status::CODE_400,
                            toString(ErrorCode::InvalidArgument),
                            std::string("malformed JSON request body: ")
                                + parsingError.what(),
                            noHeaders);

    } catch (oatpp::web::protocol::http::HttpError& httpError) {
        // oatpp's own errors, e.g. a malformed request line.
        //
        // Caught by non-const reference on purpose: oatpp declares
        // getInfo()/getMessage() without const, so a const& binding would not
        // compile. Nothing is mutated here.
        MB_LOG_DEBUG("REST: oatpp HttpError -> HTTP {}: {}",
                     httpError.getInfo().status.code, httpError.getMessage()->c_str());

        return makeResponse(httpError.getInfo().status,
                            toString(ErrorCode::InvalidArgument),
                            httpError.getMessage()->c_str(),
                            httpError.getHeaders());

    } catch (const std::exception& unexpected) {
        // Anything that is not a ServiceError is a bug in this service. The
        // client gets a generic 500; the log gets the detail.
        MB_LOG_ERROR("REST: unhandled exception: {}", unexpected.what());
        return makeResponse(http::Status::CODE_500,
                            toString(ErrorCode::Internal),
                            "internal error", noHeaders);

    } catch (...) {
        MB_LOG_ERROR("REST: unhandled non-standard exception");
        return makeResponse(http::Status::CODE_500,
                            toString(ErrorCode::Internal),
                            "internal error", noHeaders);
    }
}

std::shared_ptr<http::outgoing::Response> RestErrorHandler::handleError(
    const oatpp::web::protocol::http::Status& status,
    const oatpp::String& message,
    const Headers& headers)
{
    // Reached for framework-level problems that never became an exception -
    // most commonly an unroutable path (404) or an unparseable JSON body (400).
    const std::string text = (message == nullptr) ? std::string("request failed")
                                                  : std::string(message->c_str());

    MB_LOG_DEBUG("REST: framework error -> HTTP {}: {}", status.code, text);

    const ErrorCode code = (status.code == 404) ? ErrorCode::NotFound
                         : (status.code < 500)  ? ErrorCode::InvalidArgument
                                                : ErrorCode::Internal;

    return makeResponse(status, toString(code), text, headers);
}

} // namespace rest
} // namespace moviebackend
