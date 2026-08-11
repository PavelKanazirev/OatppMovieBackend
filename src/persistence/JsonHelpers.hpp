/**
 * @file JsonHelpers.hpp
 * @brief Small, explicit accessors over Boost.JSON.
 *
 * Boost.JSON's own accessors (`value::as_int64()` and friends) throw
 * `boost::system::system_error` with a message that tells the user nothing
 * about *which* field was wrong. Every read in this project goes through the
 * helpers below instead, so a malformed config file produces
 *
 *     config.json: field 'server.port' must be an integer
 *
 * rather than "not a number". For a file a human is expected to hand-edit,
 * that difference is the whole usability of the feature.
 *
 * This header is internal to the persistence layer - it is not installed and
 * not part of the public API.
 */
#ifndef MOVIEBACKEND_JSONHELPERS_HPP
#define MOVIEBACKEND_JSONHELPERS_HPP

#include "moviebackend/Errors.hpp"

#include <boost/json.hpp>

#include <cstdint>
#include <string>

namespace moviebackend {
namespace jsonhelpers {

/**
 * @brief Build the "origin: field 'a.b'" prefix used in every message.
 */
inline std::string where(const std::string& origin, const std::string& path)
{
    return origin + ": field '" + path + "'";
}

/**
 * @brief Parse a JSON document, reporting the origin on failure.
 * @throws ServiceError InvalidArgument if the text is not valid JSON.
 */
inline boost::json::value parseDocument(const std::string& text,
                                        const std::string& origin)
{
    boost::system::error_code errorCode;
    const boost::json::value value = boost::json::parse(text, errorCode);

    if (errorCode) {
        throw invalidArgument(origin + ": not valid JSON (" + errorCode.message() + ")");
    }
    return value;
}

/**
 * @brief Require that @p value is an object.
 *
 * Returns a pointer rather than a reference on purpose. The returned handle
 * points *into* @p value, and binding a `const object&` to the result of a
 * function call makes GCC's -Wdangling-reference fire (it cannot tell that
 * the referent outlives the call). A pointer states the aliasing plainly and
 * keeps the warning meaningful for the cases where it is a real bug.
 *
 * @return a non-owning pointer to the object inside @p value; never nullptr
 * @throws ServiceError InvalidArgument if @p value is not an object.
 */
inline const boost::json::object* requireObject(const boost::json::value& value,
                                                const std::string& origin,
                                                const std::string& path)
{
    if (!value.is_object()) {
        throw invalidArgument(where(origin, path) + " must be a JSON object");
    }
    return &value.get_object();
}

/**
 * @brief Look up a member, or nullptr when absent.
 *
 * A JSON null is treated as absent, so `"file": null` and omitting the key
 * mean the same thing.
 */
inline const boost::json::value* find(const boost::json::object& object,
                                      const std::string& key)
{
    const boost::json::value* member = object.if_contains(key);
    if (member == nullptr || member->is_null()) {
        return nullptr;
    }
    return member;
}

/** @brief Read an optional string field, returning @p fallback when absent. */
inline std::string optionalString(const boost::json::object& object,
                                  const std::string& key,
                                  const std::string& fallback,
                                  const std::string& origin,
                                  const std::string& path)
{
    const boost::json::value* member = find(object, key);
    if (member == nullptr) {
        return fallback;
    }
    if (!member->is_string()) {
        throw invalidArgument(where(origin, path) + " must be a string");
    }
    const boost::json::string& text = member->get_string();
    return std::string(text.c_str(), text.size());
}

/**
 * @brief Read a required string field.
 * @throws ServiceError InvalidArgument if absent or of the wrong type.
 */
inline std::string requiredString(const boost::json::object& object,
                                  const std::string& key,
                                  const std::string& origin,
                                  const std::string& path)
{
    const boost::json::value* member = find(object, key);
    if (member == nullptr) {
        throw invalidArgument(where(origin, path) + " is required");
    }
    if (!member->is_string()) {
        throw invalidArgument(where(origin, path) + " must be a string");
    }
    const boost::json::string& text = member->get_string();
    return std::string(text.c_str(), text.size());
}

/**
 * @brief Read an optional integer field.
 *
 * Accepts JSON integers only - a value like `20.5` for a seat capacity is a
 * mistake worth reporting rather than truncating.
 */
inline std::int64_t optionalInt(const boost::json::object& object,
                                const std::string& key,
                                std::int64_t fallback,
                                const std::string& origin,
                                const std::string& path)
{
    const boost::json::value* member = find(object, key);
    if (member == nullptr) {
        return fallback;
    }
    if (member->is_int64()) {
        return member->get_int64();
    }
    if (member->is_uint64()) {
        const std::uint64_t raw = member->get_uint64();
        if (raw > static_cast<std::uint64_t>(INT64_MAX)) {
            throw invalidArgument(where(origin, path) + " is too large");
        }
        return static_cast<std::int64_t>(raw);
    }
    throw invalidArgument(where(origin, path) + " must be an integer");
}

/** @brief Read an optional boolean field. */
inline bool optionalBool(const boost::json::object& object,
                         const std::string& key,
                         bool fallback,
                         const std::string& origin,
                         const std::string& path)
{
    const boost::json::value* member = find(object, key);
    if (member == nullptr) {
        return fallback;
    }
    if (!member->is_bool()) {
        throw invalidArgument(where(origin, path) + " must be true or false");
    }
    return member->get_bool();
}

/**
 * @brief Read an optional nested object.
 * @return a pointer to the object, or nullptr when the key is absent.
 */
inline const boost::json::object* optionalObject(const boost::json::object& object,
                                                 const std::string& key,
                                                 const std::string& origin,
                                                 const std::string& path)
{
    const boost::json::value* member = find(object, key);
    if (member == nullptr) {
        return nullptr;
    }
    if (!member->is_object()) {
        throw invalidArgument(where(origin, path) + " must be a JSON object");
    }
    return &member->get_object();
}

/**
 * @brief Read an optional array.
 * @return a pointer to the array, or nullptr when the key is absent.
 */
inline const boost::json::array* optionalArray(const boost::json::object& object,
                                               const std::string& key,
                                               const std::string& origin,
                                               const std::string& path)
{
    const boost::json::value* member = find(object, key);
    if (member == nullptr) {
        return nullptr;
    }
    if (!member->is_array()) {
        throw invalidArgument(where(origin, path) + " must be a JSON array");
    }
    return &member->get_array();
}

/**
 * @brief Narrow a std::int64_t to int, with a range check.
 *
 * The JSON layer reads 64-bit integers; the domain uses plain int. This is
 * the one place that conversion happens, and it refuses rather than wraps.
 */
inline int toInt(std::int64_t value, const std::string& origin, const std::string& path)
{
    if (value < INT32_MIN || value > INT32_MAX) {
        throw invalidArgument(where(origin, path) + " is out of range");
    }
    return static_cast<int>(value);
}

} // namespace jsonhelpers
} // namespace moviebackend

#endif // MOVIEBACKEND_JSONHELPERS_HPP
