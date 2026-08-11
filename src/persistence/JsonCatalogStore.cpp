/**
 * @file JsonCatalogStore.cpp
 * @brief Reading and writing the catalog JSON file with Boost.JSON.
 */
#include "moviebackend/CatalogStore.hpp"

#include "moviebackend/Errors.hpp"
#include "moviebackend/Logging.hpp"
#include "moviebackend/Schedule.hpp"
#include "persistence/JsonHelpers.hpp"

#include <boost/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace moviebackend {

namespace {

/** @brief Read a whole file into a string, or throw PersistenceFailure. */
std::string readWholeFile(const std::string& path)
{
    // Check that this is a regular file *before* opening it. On Linux,
    // std::ifstream will happily "open" a directory - is_open() returns true
    // and the read then silently yields nothing. Without this guard a catalog
    // path that points at a directory would surface as a confusing "not valid
    // JSON" error instead of naming the real problem.
    std::error_code statError;
    if (!std::filesystem::is_regular_file(path, statError)) {
        throw ServiceError(ErrorCode::PersistenceFailure,
                           "'" + path + "' is not a regular file");
    }

    std::ifstream stream(path.c_str(), std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        throw ServiceError(ErrorCode::PersistenceFailure,
                           "cannot open '" + path + "' for reading");
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();

    if (stream.bad()) {
        throw ServiceError(ErrorCode::PersistenceFailure,
                           "error while reading '" + path + "'");
    }
    return buffer.str();
}

/* -------------------------------------------------------------------------
 * Deserialisation helpers - one per entity, all following the same shape.
 * ------------------------------------------------------------------------- */

Movie parseMovie(const boost::json::value& value,
                 const std::string& origin,
                 const std::string& path)
{
    using namespace jsonhelpers;

    const boost::json::object& object = *requireObject(value, origin, path);

    Movie movie;
    movie.id = requiredString(object, "id", origin, path + ".id");
    movie.title = requiredString(object, "title", origin, path + ".title");
    movie.durationMinutes = toInt(
        optionalInt(object, "duration_minutes", 0, origin, path + ".duration_minutes"),
        origin, path + ".duration_minutes");
    movie.language = optionalString(object, "language", "", origin, path + ".language");
    movie.genre = optionalString(object, "genre", "", origin, path + ".genre");

    return movie;
}

Theater parseTheater(const boost::json::value& value,
                     const TheaterDefaults& defaults,
                     const std::string& origin,
                     const std::string& path)
{
    using namespace jsonhelpers;

    const boost::json::object& object = *requireObject(value, origin, path);

    Theater theater;
    theater.id = requiredString(object, "id", origin, path + ".id");
    theater.name = requiredString(object, "name", origin, path + ".name");
    theater.city = optionalString(object, "city", "", origin, path + ".city");

    // Omitting seat_capacity is how a catalog entry asks for the configured
    // default of 20.
    theater.seatCapacity = toInt(
        optionalInt(object, "seat_capacity", defaults.seatCapacity,
                    origin, path + ".seat_capacity"),
        origin, path + ".seat_capacity");
    theater.seatsPerRow = toInt(
        optionalInt(object, "seats_per_row", defaults.seatsPerRow,
                    origin, path + ".seats_per_row"),
        origin, path + ".seats_per_row");

    return theater;
}

ShowtimeSnapshot parseShowtime(const boost::json::value& value,
                               const TheaterDefaults& defaults,
                               const std::string& origin,
                               const std::string& path)
{
    using namespace jsonhelpers;

    const boost::json::object& object = *requireObject(value, origin, path);

    ShowtimeSnapshot entry;
    entry.showtime.id = requiredString(object, "id", origin, path + ".id");
    entry.showtime.movieId = requiredString(object, "movie_id", origin, path + ".movie_id");
    entry.showtime.theaterId = requiredString(object, "theater_id", origin,
                                              path + ".theater_id");

    const std::string startText =
        requiredString(object, "start_time", origin, path + ".start_time");
    entry.showtime.startTime = schedule::parseTimeOfDay(startText);

    entry.showtime.seatCapacity = toInt(
        optionalInt(object, "seat_capacity", defaults.seatCapacity,
                    origin, path + ".seat_capacity"),
        origin, path + ".seat_capacity");
    entry.showtime.seatsPerRow = toInt(
        optionalInt(object, "seats_per_row", defaults.seatsPerRow,
                    origin, path + ".seats_per_row"),
        origin, path + ".seats_per_row");

    const boost::json::array* booked =
        optionalArray(object, "booked_seats", origin, path + ".booked_seats");
    if (booked != nullptr) {
        for (std::size_t i = 0; i < booked->size(); ++i) {
            const boost::json::value& seat = (*booked)[i];
            if (!seat.is_string()) {
                throw invalidArgument(where(origin, path + ".booked_seats")
                                      + " must contain only strings");
            }
            const boost::json::string& text = seat.get_string();
            entry.bookedSeats.push_back(std::string(text.c_str(), text.size()));
        }
    }

    return entry;
}

Booking parseBooking(const boost::json::value& value,
                     const std::string& origin,
                     const std::string& path)
{
    using namespace jsonhelpers;

    const boost::json::object& object = *requireObject(value, origin, path);

    Booking booking;
    booking.id = requiredString(object, "id", origin, path + ".id");
    booking.showtimeId = requiredString(object, "showtime_id", origin,
                                        path + ".showtime_id");
    booking.customerName = optionalString(object, "customer_name", "", origin,
                                          path + ".customer_name");
    booking.createdAtEpochSeconds =
        optionalInt(object, "created_at", 0, origin, path + ".created_at");

    const boost::json::array* seats =
        optionalArray(object, "seats", origin, path + ".seats");
    if (seats != nullptr) {
        for (std::size_t i = 0; i < seats->size(); ++i) {
            const boost::json::value& seat = (*seats)[i];
            if (!seat.is_string()) {
                throw invalidArgument(where(origin, path + ".seats")
                                      + " must contain only strings");
            }
            const boost::json::string& text = seat.get_string();
            booking.seats.push_back(std::string(text.c_str(), text.size()));
        }
    }

    return booking;
}

/* -------------------------------------------------------------------------
 * Serialisation helpers
 * ------------------------------------------------------------------------- */

boost::json::array toJsonArray(const std::vector<SeatLabel>& labels)
{
    boost::json::array result;
    result.reserve(labels.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
        result.push_back(boost::json::value(labels[i]));
    }
    return result;
}

/**
 * @brief Pretty-print a Boost.JSON value.
 *
 * Boost.JSON's serializer emits one dense line. The catalog file is meant to
 * be hand-edited, so it is printed with indentation instead. Written as an
 * explicit recursive function rather than pulled from a library because it is
 * short and keeps the dependency surface at exactly "Boost.JSON".
 */
void prettyPrint(std::ostream& out, const boost::json::value& value, int indent)
{
    const std::string pad(static_cast<std::size_t>(indent) * 2U, ' ');
    const std::string padInner(static_cast<std::size_t>(indent + 1) * 2U, ' ');

    switch (value.kind()) {
        case boost::json::kind::object: {
            const boost::json::object& object = value.get_object();
            if (object.empty()) {
                out << "{}";
                return;
            }
            out << "{\n";
            bool first = true;
            for (boost::json::object::const_iterator it = object.begin();
                 it != object.end(); ++it) {
                if (!first) {
                    out << ",\n";
                }
                first = false;
                out << padInner << boost::json::serialize(it->key()) << ": ";
                prettyPrint(out, it->value(), indent + 1);
            }
            out << "\n" << pad << "}";
            return;
        }
        case boost::json::kind::array: {
            const boost::json::array& array = value.get_array();
            if (array.empty()) {
                out << "[]";
                return;
            }
            out << "[\n";
            for (std::size_t i = 0; i < array.size(); ++i) {
                if (i > 0) {
                    out << ",\n";
                }
                out << padInner;
                prettyPrint(out, array[i], indent + 1);
            }
            out << "\n" << pad << "]";
            return;
        }
        default:
            out << boost::json::serialize(value);
            return;
    }
}

} // unnamed namespace

/* =========================================================================
 * JsonCatalogStore
 * ========================================================================= */

JsonCatalogStore::JsonCatalogStore(std::string path, const TheaterDefaults& defaults)
    : m_path(std::move(path))
    , m_defaults(defaults)
{
}

CatalogSnapshot JsonCatalogStore::parse(const std::string& json,
                                        const TheaterDefaults& defaults,
                                        const std::string& originForErrors)
{
    using namespace jsonhelpers;

    CatalogSnapshot snapshot;

    const boost::json::value document = parseDocument(json, originForErrors);
    const boost::json::object& root = *requireObject(document, originForErrors, "<root>");

    const boost::json::array* movies = optionalArray(root, "movies", originForErrors, "movies");
    if (movies != nullptr) {
        for (std::size_t i = 0; i < movies->size(); ++i) {
            snapshot.movies.push_back(parseMovie(
                (*movies)[i], originForErrors, "movies[" + std::to_string(i) + "]"));
        }
    }

    const boost::json::array* theaters =
        optionalArray(root, "theaters", originForErrors, "theaters");
    if (theaters != nullptr) {
        for (std::size_t i = 0; i < theaters->size(); ++i) {
            snapshot.theaters.push_back(parseTheater(
                (*theaters)[i], defaults, originForErrors,
                "theaters[" + std::to_string(i) + "]"));
        }
    }

    const boost::json::array* showtimes =
        optionalArray(root, "showtimes", originForErrors, "showtimes");
    if (showtimes != nullptr) {
        for (std::size_t i = 0; i < showtimes->size(); ++i) {
            snapshot.showtimes.push_back(parseShowtime(
                (*showtimes)[i], defaults, originForErrors,
                "showtimes[" + std::to_string(i) + "]"));
        }
    }

    const boost::json::array* bookings =
        optionalArray(root, "bookings", originForErrors, "bookings");
    if (bookings != nullptr) {
        for (std::size_t i = 0; i < bookings->size(); ++i) {
            snapshot.bookings.push_back(parseBooking(
                (*bookings)[i], originForErrors, "bookings[" + std::to_string(i) + "]"));
        }
    }

    return snapshot;
}

std::string JsonCatalogStore::serialise(const CatalogSnapshot& snapshot)
{
    boost::json::object root;

    boost::json::array movies;
    movies.reserve(snapshot.movies.size());
    for (std::size_t i = 0; i < snapshot.movies.size(); ++i) {
        const Movie& movie = snapshot.movies[i];
        boost::json::object entry;
        entry["id"] = movie.id;
        entry["title"] = movie.title;
        entry["duration_minutes"] = movie.durationMinutes;
        entry["language"] = movie.language;
        entry["genre"] = movie.genre;
        movies.push_back(boost::json::value(std::move(entry)));
    }
    root["movies"] = std::move(movies);

    boost::json::array theaters;
    theaters.reserve(snapshot.theaters.size());
    for (std::size_t i = 0; i < snapshot.theaters.size(); ++i) {
        const Theater& theater = snapshot.theaters[i];
        boost::json::object entry;
        entry["id"] = theater.id;
        entry["name"] = theater.name;
        entry["city"] = theater.city;
        entry["seat_capacity"] = theater.seatCapacity;
        entry["seats_per_row"] = theater.seatsPerRow;
        theaters.push_back(boost::json::value(std::move(entry)));
    }
    root["theaters"] = std::move(theaters);

    boost::json::array showtimes;
    showtimes.reserve(snapshot.showtimes.size());
    for (std::size_t i = 0; i < snapshot.showtimes.size(); ++i) {
        const ShowtimeSnapshot& entry = snapshot.showtimes[i];
        boost::json::object object;
        object["id"] = entry.showtime.id;
        object["movie_id"] = entry.showtime.movieId;
        object["theater_id"] = entry.showtime.theaterId;
        object["start_time"] = schedule::formatTimeOfDay(entry.showtime.startTime);
        object["seat_capacity"] = entry.showtime.seatCapacity;
        object["seats_per_row"] = entry.showtime.seatsPerRow;
        object["booked_seats"] = toJsonArray(entry.bookedSeats);
        showtimes.push_back(boost::json::value(std::move(object)));
    }
    root["showtimes"] = std::move(showtimes);

    boost::json::array bookings;
    bookings.reserve(snapshot.bookings.size());
    for (std::size_t i = 0; i < snapshot.bookings.size(); ++i) {
        const Booking& booking = snapshot.bookings[i];
        boost::json::object entry;
        entry["id"] = booking.id;
        entry["showtime_id"] = booking.showtimeId;
        entry["seats"] = toJsonArray(booking.seats);
        entry["customer_name"] = booking.customerName;
        entry["created_at"] = booking.createdAtEpochSeconds;
        bookings.push_back(boost::json::value(std::move(entry)));
    }
    root["bookings"] = std::move(bookings);

    std::ostringstream out;
    prettyPrint(out, boost::json::value(std::move(root)), 0);
    out << "\n";
    return out.str();
}

CatalogSnapshot JsonCatalogStore::load()
{
    // A missing catalog file means "start empty", not "fail to boot". That is
    // what makes a first run on a clean checkout work.
    std::error_code existsError;
    if (!std::filesystem::exists(m_path, existsError)) {
        MB_LOG_WARN("catalog file '{}' does not exist - starting with an empty catalog",
                    m_path);
        return CatalogSnapshot();
    }

    MB_LOG_INFO("loading catalog from '{}'", m_path);
    return parse(readWholeFile(m_path), m_defaults, m_path);
}

void JsonCatalogStore::save(const CatalogSnapshot& snapshot)
{
    const std::string text = serialise(snapshot);
    const std::string temporaryPath = m_path + ".tmp";

    // --- write to a temporary file first ---------------------------------
    {
        std::ofstream stream(temporaryPath.c_str(),
                             std::ios::out | std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            throw ServiceError(ErrorCode::PersistenceFailure,
                               "cannot open '" + temporaryPath + "' for writing");
        }

        stream << text;
        stream.flush();

        if (!stream.good()) {
            throw ServiceError(ErrorCode::PersistenceFailure,
                               "error while writing '" + temporaryPath + "'");
        }
    } // the stream is closed here, before the rename

    // --- then move it into place atomically ------------------------------
    //
    // std::filesystem::rename replaces the destination on POSIX. On Windows
    // it fails if the destination exists, so the old file is removed first.
    // That leaves a very small window with no catalog file, which is
    // acceptable: the file is a checkpoint, and the authoritative state is in
    // memory.
    std::error_code renameError;

#ifdef _WIN32
    std::error_code removeError;
    std::filesystem::remove(m_path, removeError);
#endif

    std::filesystem::rename(temporaryPath, m_path, renameError);

    if (renameError) {
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        throw ServiceError(ErrorCode::PersistenceFailure,
                           "cannot move '" + temporaryPath + "' onto '" + m_path
                               + "': " + renameError.message());
    }

    MB_LOG_DEBUG("catalog saved to '{}' ({} bytes)", m_path, text.size());
}

} // namespace moviebackend
