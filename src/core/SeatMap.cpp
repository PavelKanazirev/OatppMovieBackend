/**
 * @file SeatMap.cpp
 * @brief Seat occupancy and the seat-label scheme.
 */
#include "moviebackend/SeatMap.hpp"

#include "moviebackend/Errors.hpp"

#include <cctype>
#include <string>

namespace moviebackend {

namespace {

/** We label rows 'a'..'z', so a layout may not need more than 26 rows. */
constexpr int kMaxRows = 26;

/**
 * @brief Hard upper bound on any layout dimension.
 *
 * A real auditorium has a few thousand seats at most. The bound exists for
 * two concrete reasons, both reachable from an unauthenticated admin request:
 *
 *   * it stops `seat_capacity: 2000000000` turning into a multi-gigabyte
 *     `std::vector` allocation, and
 *   * together with the 64-bit arithmetic below it makes the index
 *     calculations provably overflow-free.
 */
constexpr int kMaxLayoutDimension = 100000;

/**
 * @brief Number of rows needed for @p capacity seats at @p seatsPerRow each.
 *
 * Integer ceiling division, written out rather than using a helper so the
 * arithmetic is visible.
 *
 * Deliberately done in 64-bit: `(capacity + seatsPerRow - 1)` overflows a
 * 32-bit int for large inputs, and a wrapped negative numerator yields a
 * quotient of 0, which would sail straight past the 26-row check below.
 */
long long rowsNeeded(long long capacity, long long seatsPerRow)
{
    return (capacity + seatsPerRow - 1) / seatsPerRow;
}

/**
 * @brief Validate a layout, throwing if it cannot be labelled or stored.
 */
void validateLayout(int capacity, int seatsPerRow)
{
    if (capacity <= 0) {
        throw invalidArgument("seat capacity must be positive, got "
                              + std::to_string(capacity));
    }
    if (seatsPerRow <= 0) {
        throw invalidArgument("seats per row must be positive, got "
                              + std::to_string(seatsPerRow));
    }
    if (capacity > kMaxLayoutDimension) {
        throw invalidArgument("seat capacity must not exceed "
                              + std::to_string(kMaxLayoutDimension) + ", got "
                              + std::to_string(capacity));
    }
    if (seatsPerRow > kMaxLayoutDimension) {
        throw invalidArgument("seats per row must not exceed "
                              + std::to_string(kMaxLayoutDimension) + ", got "
                              + std::to_string(seatsPerRow));
    }
    if (rowsNeeded(capacity, seatsPerRow) > kMaxRows) {
        throw invalidArgument(
            "layout needs more than 26 rows (capacity "
            + std::to_string(capacity) + ", "
            + std::to_string(seatsPerRow) + " per row); rows are labelled a..z");
    }
}

} // unnamed namespace

SeatMap::SeatMap(int capacity, int seatsPerRow)
    : m_capacity(capacity)
    , m_seatsPerRow(seatsPerRow)
    , m_seats()
{
    validateLayout(capacity, seatsPerRow);
    m_seats.assign(static_cast<std::size_t>(capacity), SeatState::Free);
}

std::optional<std::size_t> SeatMap::labelToIndex(const SeatLabel& label) const
{
    // A label is one row letter followed by a 1-based seat number: "a1", "b10".
    if (label.size() < 2) {
        return std::nullopt;
    }

    const unsigned char rawRow = static_cast<unsigned char>(label[0]);
    const int rowChar = std::tolower(rawRow);

    if (rowChar < 'a' || rowChar > 'z') {
        return std::nullopt;
    }
    const int rowIndex = rowChar - 'a';

    // Parse the seat number. Leading zeros ("a01") are rejected so that every
    // seat has exactly one spelling - otherwise "a1" and "a01" would be two
    // names for one seat and the duplicate check in bookSeats could be fooled.
    if (label[1] == '0') {
        return std::nullopt;
    }

    int seatNumber = 0;
    for (std::size_t i = 1; i < label.size(); ++i) {
        const char c = label[i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        seatNumber = seatNumber * 10 + (c - '0');
        if (seatNumber > m_capacity) {
            return std::nullopt; // cannot possibly be a seat here
        }
    }

    if (seatNumber < 1 || seatNumber > m_seatsPerRow) {
        return std::nullopt;
    }

    // 64-bit arithmetic. validateLayout bounds both dimensions, so this
    // cannot overflow - but computing it in `int` would have: rowIndex is up
    // to 25, and 25 * a large seatsPerRow wraps negative, which then passes a
    // `>= m_capacity` test and gets cast to an enormous std::size_t. That is
    // an out-of-bounds subscript that reads as perfectly ordinary code.
    const long long flatIndex =
        static_cast<long long>(rowIndex) * static_cast<long long>(m_seatsPerRow)
        + static_cast<long long>(seatNumber - 1);

    if (flatIndex < 0 || flatIndex >= static_cast<long long>(m_capacity)) {
        // Inside the row grid but past the end of a partially filled last row.
        return std::nullopt;
    }

    return static_cast<std::size_t>(flatIndex);
}

SeatLabel SeatMap::indexToLabel(std::size_t index) const
{
    if (index >= static_cast<std::size_t>(m_capacity)) {
        throw invalidArgument("seat index out of range: " + std::to_string(index));
    }

    const int flat = static_cast<int>(index);
    const int rowIndex   = flat / m_seatsPerRow;
    const int seatNumber = flat % m_seatsPerRow + 1;

    SeatLabel label;
    label.push_back(static_cast<char>('a' + rowIndex));
    label += std::to_string(seatNumber);
    return label;
}

bool SeatMap::isFree(const SeatLabel& label) const
{
    const std::optional<std::size_t> index = labelToIndex(label);
    if (!index.has_value()) {
        return false;
    }
    return m_seats[*index] == SeatState::Free;
}

void SeatMap::book(const SeatLabel& label)
{
    const std::optional<std::size_t> index = labelToIndex(label);
    if (!index.has_value()) {
        throw notFound("seat", label);
    }
    if (m_seats[*index] != SeatState::Free) {
        throw ServiceError(ErrorCode::SeatUnavailable,
                           "seat '" + label + "' is already booked");
    }
    m_seats[*index] = SeatState::Booked;
}

void SeatMap::release(const SeatLabel& label)
{
    const std::optional<std::size_t> index = labelToIndex(label);
    if (!index.has_value()) {
        throw notFound("seat", label);
    }
    m_seats[*index] = SeatState::Free;
}

std::vector<SeatLabel> SeatMap::availableSeats() const
{
    std::vector<SeatLabel> result;
    result.reserve(m_seats.size());

    for (std::size_t i = 0; i < m_seats.size(); ++i) {
        if (m_seats[i] == SeatState::Free) {
            result.push_back(indexToLabel(i));
        }
    }
    return result;
}

std::vector<SeatLabel> SeatMap::bookedSeats() const
{
    std::vector<SeatLabel> result;

    for (std::size_t i = 0; i < m_seats.size(); ++i) {
        if (m_seats[i] == SeatState::Booked) {
            result.push_back(indexToLabel(i));
        }
    }
    return result;
}

int SeatMap::availableCount() const noexcept
{
    int count = 0;
    for (std::size_t i = 0; i < m_seats.size(); ++i) {
        if (m_seats[i] == SeatState::Free) {
            ++count;
        }
    }
    return count;
}

void SeatMap::relayout(int newCapacity, int newSeatsPerRow)
{
    validateLayout(newCapacity, newSeatsPerRow);

    // Remember what is booked *by label*, because the labels are what the
    // customer holds. Re-applying them to the new grid is what preserves a
    // booking across a layout change.
    const std::vector<SeatLabel> booked = bookedSeats();

    SeatMap replacement(newCapacity, newSeatsPerRow);

    for (std::size_t i = 0; i < booked.size(); ++i) {
        const std::optional<std::size_t> index =
            replacement.labelToIndex(booked[i]);

        if (!index.has_value()) {
            // The seat would vanish. Refuse the whole operation - *this is
            // left completely untouched because everything so far happened on
            // the replacement copy. That is the strong exception guarantee,
            // achieved simply by building the new state to one side first.
            throw conflict("cannot change layout to capacity "
                           + std::to_string(newCapacity) + " / "
                           + std::to_string(newSeatsPerRow)
                           + " per row: seat '" + booked[i]
                           + "' is booked and would no longer exist");
        }
        replacement.m_seats[*index] = SeatState::Booked;
    }

    *this = replacement;
}

} // namespace moviebackend
