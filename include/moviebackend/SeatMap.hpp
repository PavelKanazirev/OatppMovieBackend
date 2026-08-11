/**
 * @file SeatMap.hpp
 * @brief Seat occupancy for a single showtime, plus the label <-> index
 *        translation.
 *
 * SeatMap knows nothing about threads. It is always used from inside the
 * Catalog lock, and keeping it lock-free (in the "contains no locking" sense,
 * not the atomics sense) makes it trivial to unit test.
 */
#ifndef MOVIEBACKEND_SEATMAP_HPP
#define MOVIEBACKEND_SEATMAP_HPP

#include "moviebackend/Domain.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace moviebackend {

/**
 * @brief Occupancy bitmap for one showtime.
 *
 * ## Labelling scheme
 *
 * Seats are laid out row by row. With @p seatsPerRow = 10:
 *
 *     index 0..9   -> a1 .. a10
 *     index 10..19 -> b1 .. b10
 *
 * The last row may be partially filled if capacity is not a multiple of
 * seatsPerRow (capacity 12, 10 per row -> a1..a10, b1, b2).
 *
 * ## Note on the storage type
 *
 * The occupancy vector is `std::vector<SeatState>` and *not*
 * `std::vector<bool>`. The bool specialisation is the classic C++ trap: it is
 * a proxy container, you cannot take a reference to an element, and it makes
 * the code harder to reason about for no benefit at these sizes.
 */
class SeatMap {
public:
    /** @brief Occupancy of a single seat. */
    enum class SeatState : unsigned char {
        Free = 0,
        Booked = 1
    };

    /**
     * @brief Construct an all-free seat map.
     * @param capacity    total number of seats, must be > 0
     * @param seatsPerRow seats in a full row, must be > 0
     * @throws ServiceError InvalidArgument if either argument is <= 0, or if
     *         the layout would need more than 26 rows (we only label a..z).
     */
    SeatMap(int capacity, int seatsPerRow);

    /** @return total number of seats in this map. */
    int capacity() const noexcept { return m_capacity; }

    /** @return seats per full row. */
    int seatsPerRow() const noexcept { return m_seatsPerRow; }

    /**
     * @brief Translate a seat label to its index.
     * @param label e.g. "a1", "B7" (matching is case-insensitive)
     * @return the index, or std::nullopt if the label is malformed or refers
     *         to a seat outside this map.
     *
     * Modern C++ note: `std::optional` is the C++17 way of saying "a value
     * that might not be there" without sentinel values or output parameters.
     * It is used here because "is this a real seat?" is a question the caller
     * must answer before doing anything else, and a sentinel index would be
     * easy to forget to check.
     */
    std::optional<std::size_t> labelToIndex(const SeatLabel& label) const;

    /**
     * @brief Translate an index back to its canonical lower-case label.
     * @param index seat index, must be < capacity()
     * @throws ServiceError InvalidArgument if the index is out of range.
     */
    SeatLabel indexToLabel(std::size_t index) const;

    /**
     * @brief Is the seat with this label free?
     * @param label seat label
     * @return true only if the label is valid *and* the seat is unbooked.
     */
    bool isFree(const SeatLabel& label) const;

    /**
     * @brief Mark a seat as booked.
     * @param label seat label
     * @throws ServiceError NotFound if the label does not name a seat here,
     *         SeatUnavailable if it is already booked.
     */
    void book(const SeatLabel& label);

    /**
     * @brief Release a previously booked seat (used when a booking is
     *        cancelled and when a showtime is rebuilt from disk).
     * @param label seat label
     * @throws ServiceError NotFound if the label does not name a seat here.
     */
    void release(const SeatLabel& label);

    /** @return every free seat, in row order. */
    std::vector<SeatLabel> availableSeats() const;

    /** @return every booked seat, in row order. */
    std::vector<SeatLabel> bookedSeats() const;

    /** @return how many seats are currently free. */
    int availableCount() const noexcept;

    /**
     * @brief Change the layout, preserving existing bookings.
     *
     * Used when an administrator edits a theater. Growing is always safe.
     * Shrinking is refused if any seat that would disappear is booked -
     * silently dropping a paid-for seat is exactly the kind of failure this
     * service exists to prevent.
     *
     * Re-labelling (changing seatsPerRow) is treated the same way: the booked
     * *labels* are preserved, so a seat booked as "b1" stays "b1" as long as
     * "b1" still exists in the new layout.
     *
     * @param newCapacity    new total seat count
     * @param newSeatsPerRow new row width
     * @throws ServiceError InvalidArgument for a non-positive layout,
     *         Conflict if a booked seat would be lost.
     */
    void relayout(int newCapacity, int newSeatsPerRow);

private:
    int                    m_capacity;
    int                    m_seatsPerRow;
    std::vector<SeatState> m_seats;
};

} // namespace moviebackend

#endif // MOVIEBACKEND_SEATMAP_HPP
