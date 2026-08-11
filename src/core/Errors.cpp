/**
 * @file Errors.cpp
 * @brief Implementation of the error taxonomy helpers.
 */
#include "moviebackend/Errors.hpp"

namespace moviebackend {

const char* toString(ErrorCode code) noexcept
{
    // A plain switch with no default: if somebody adds an ErrorCode later,
    // the compiler warns about the unhandled case instead of silently
    // returning "UNKNOWN" at run time. That is the whole reason there is no
    // default label here.
    switch (code) {
        case ErrorCode::NotFound:           return "NOT_FOUND";
        case ErrorCode::InvalidArgument:    return "INVALID_ARGUMENT";
        case ErrorCode::Conflict:           return "CONFLICT";
        case ErrorCode::SeatUnavailable:    return "SEAT_UNAVAILABLE";
        case ErrorCode::PersistenceFailure: return "PERSISTENCE_FAILURE";
        case ErrorCode::Internal:           return "INTERNAL";
    }
    return "INTERNAL";
}

ServiceError notFound(const std::string& entity, const std::string& id)
{
    return ServiceError(ErrorCode::NotFound,
                        "no such " + entity + ": '" + id + "'");
}

ServiceError invalidArgument(const std::string& message)
{
    return ServiceError(ErrorCode::InvalidArgument, message);
}

ServiceError conflict(const std::string& message)
{
    return ServiceError(ErrorCode::Conflict, message);
}

} // namespace moviebackend
