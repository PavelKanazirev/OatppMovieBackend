/**
 * @file SeatMapTest.cpp
 * @brief Tests for seat labelling and occupancy.
 *
 * The label scheme is the part of the domain most likely to be got subtly
 * wrong (off-by-one on the 1-based seat number, partially filled last rows),
 * so it gets the most detailed tests in the suite.
 */
#include "moviebackend/SeatMap.hpp"

#include "moviebackend/Errors.hpp"
#include "TestSupport.hpp"

#include <gtest/gtest.h>

using moviebackend::ErrorCode;
using moviebackend::SeatLabel;
using moviebackend::SeatMap;

namespace {

TEST(SeatMapTest, DefaultCapacityOfTwentyLabelsTwoRows)
{
    // The capacity required by the exercise, with the default row width.
    const SeatMap seats(20, 10);

    EXPECT_EQ(20, seats.capacity());
    EXPECT_EQ(20, seats.availableCount());

    const std::vector<SeatLabel> available = seats.availableSeats();
    ASSERT_EQ(20u, available.size());

    EXPECT_EQ("a1", available.front());
    EXPECT_EQ("a10", available[9]);
    EXPECT_EQ("b1", available[10]);
    EXPECT_EQ("b10", available.back());
}

TEST(SeatMapTest, LabelToIndexRoundTrips)
{
    const SeatMap seats(20, 10);

    for (std::size_t i = 0; i < 20; ++i) {
        const SeatLabel label = seats.indexToLabel(i);
        const std::optional<std::size_t> index = seats.labelToIndex(label);

        ASSERT_TRUE(index.has_value()) << "label '" << label << "' did not map back";
        EXPECT_EQ(i, *index);
    }
}

TEST(SeatMapTest, LabelLookupIsCaseInsensitive)
{
    const SeatMap seats(20, 10);

    EXPECT_EQ(seats.labelToIndex("a1"), seats.labelToIndex("A1"));
    EXPECT_TRUE(seats.labelToIndex("B7").has_value());
}

TEST(SeatMapTest, RejectsMalformedAndOutOfRangeLabels)
{
    const SeatMap seats(20, 10);

    EXPECT_FALSE(seats.labelToIndex("").has_value());
    EXPECT_FALSE(seats.labelToIndex("a").has_value());
    EXPECT_FALSE(seats.labelToIndex("1a").has_value());
    EXPECT_FALSE(seats.labelToIndex("a0").has_value());     // seats are 1-based
    EXPECT_FALSE(seats.labelToIndex("a01").has_value());    // one spelling per seat
    EXPECT_FALSE(seats.labelToIndex("a11").has_value());    // beyond the row width
    EXPECT_FALSE(seats.labelToIndex("c1").has_value());     // beyond the last row
    EXPECT_FALSE(seats.labelToIndex("a1x").has_value());
    EXPECT_FALSE(seats.labelToIndex("!1").has_value());
}

TEST(SeatMapTest, PartiallyFilledLastRowStopsAtCapacity)
{
    // 12 seats at 10 per row: a1..a10 then b1, b2 - and b3 must not exist.
    const SeatMap seats(12, 10);

    EXPECT_TRUE(seats.labelToIndex("b2").has_value());
    EXPECT_FALSE(seats.labelToIndex("b3").has_value());
    EXPECT_EQ(12u, seats.availableSeats().size());
}

TEST(SeatMapTest, BookingMarksASeatAndIsNotRepeatable)
{
    SeatMap seats(20, 10);

    seats.book("a1");

    EXPECT_FALSE(seats.isFree("a1"));
    EXPECT_TRUE(seats.isFree("a2"));
    EXPECT_EQ(19, seats.availableCount());

    // The second attempt is the over-booking guard at its lowest level.
    EXPECT_SERVICE_ERROR(seats.book("a1"), ErrorCode::SeatUnavailable);
}

TEST(SeatMapTest, BookingAnUnknownSeatIsNotFound)
{
    SeatMap seats(20, 10);
    EXPECT_SERVICE_ERROR(seats.book("z9"), ErrorCode::NotFound);
}

TEST(SeatMapTest, ReleaseFreesASeat)
{
    SeatMap seats(20, 10);

    seats.book("a1");
    seats.release("a1");

    EXPECT_TRUE(seats.isFree("a1"));
    EXPECT_EQ(20, seats.availableCount());
}

TEST(SeatMapTest, BookedSeatsAreListedInRowOrder)
{
    SeatMap seats(20, 10);

    seats.book("b2");
    seats.book("a3");

    const std::vector<SeatLabel> booked = seats.bookedSeats();
    ASSERT_EQ(2u, booked.size());
    EXPECT_EQ("a3", booked[0]);
    EXPECT_EQ("b2", booked[1]);
}

TEST(SeatMapTest, RejectsUnlabellableLayouts)
{
    EXPECT_SERVICE_ERROR(SeatMap(0, 10), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(SeatMap(-5, 10), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(SeatMap(20, 0), ErrorCode::InvalidArgument);

    // 27 rows of one seat each - one row past 'z'.
    EXPECT_SERVICE_ERROR(SeatMap(27, 1), ErrorCode::InvalidArgument);

    // Exactly 26 rows is still fine.
    EXPECT_NO_THROW(SeatMap(26, 1));
}

TEST(SeatMapTest, RejectsLayoutsThatWouldOverflowTheIndexArithmetic)
{
    // Regression guard. These all used to be accepted, and each one then
    // corrupted memory in a different way:
    //
    //  * (1, 90000000) passed the 26-row check (one row), and then
    //    "z1" computed 25 * 90000000, wrapped negative, slipped past the
    //    "< capacity" guard, and indexed the vector far out of bounds.
    //  * (2000000000, 2000000000) overflowed the ceiling division to a
    //    quotient of 0 rows, then tried to allocate 2 GB.
    EXPECT_SERVICE_ERROR(SeatMap(1, 90000000), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(SeatMap(2000000000, 2000000000), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(SeatMap(2000000000, 10), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(SeatMap(200000, 100000), ErrorCode::InvalidArgument);

    // A large but sane hall is still fine.
    EXPECT_NO_THROW(SeatMap(2600, 100));
}

TEST(SeatMapTest, EverySeatOfALargeLayoutResolvesInBounds)
{
    // Walks the whole grid to prove no label maps outside the vector.
    const SeatMap seats(2600, 100);

    const std::vector<SeatLabel> all = seats.availableSeats();
    ASSERT_EQ(2600u, all.size());

    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::optional<std::size_t> index = seats.labelToIndex(all[i]);
        ASSERT_TRUE(index.has_value()) << all[i];
        EXPECT_LT(*index, 2600u);
    }

    // Row 'z' is the 26th row: 25 * 100 = 2500, so z100 is the last seat.
    EXPECT_TRUE(seats.labelToIndex("z100").has_value());
    EXPECT_FALSE(seats.labelToIndex("z101").has_value());
}

TEST(SeatMapTest, GrowingKeepsExistingBookings)
{
    SeatMap seats(20, 10);
    seats.book("b5");

    seats.relayout(40, 10);

    EXPECT_EQ(40, seats.capacity());
    EXPECT_FALSE(seats.isFree("b5"));
    EXPECT_EQ(39, seats.availableCount());
}

TEST(SeatMapTest, ShrinkingIsRefusedWhenItWouldDropABookedSeat)
{
    SeatMap seats(20, 10);
    seats.book("b5");   // index 14

    EXPECT_SERVICE_ERROR(seats.relayout(10, 10), ErrorCode::Conflict);

    // The refusal must leave the map completely untouched.
    EXPECT_EQ(20, seats.capacity());
    EXPECT_FALSE(seats.isFree("b5"));
    EXPECT_EQ(19, seats.availableCount());
}

TEST(SeatMapTest, ShrinkingIsAllowedWhenNoBookedSeatIsLost)
{
    SeatMap seats(20, 10);
    seats.book("a1");

    seats.relayout(10, 10);

    EXPECT_EQ(10, seats.capacity());
    EXPECT_FALSE(seats.isFree("a1"));
    EXPECT_EQ(9, seats.availableCount());
}

TEST(SeatMapTest, ChangingRowWidthPreservesLabelsThatStillExist)
{
    SeatMap seats(20, 10);
    seats.book("a2");

    // 5 per row: a1..a5, b1..b5, c1..c5, d1..d5. "a2" still exists.
    seats.relayout(20, 5);

    EXPECT_FALSE(seats.isFree("a2"));
    EXPECT_TRUE(seats.labelToIndex("d5").has_value());
    EXPECT_FALSE(seats.labelToIndex("a6").has_value());
}

TEST(SeatMapTest, ChangingRowWidthIsRefusedWhenALabelWouldVanish)
{
    SeatMap seats(20, 10);
    seats.book("a9");   // "a9" does not exist in a 5-wide layout

    EXPECT_SERVICE_ERROR(seats.relayout(20, 5), ErrorCode::Conflict);
    EXPECT_EQ(10, seats.seatsPerRow());
}

} // unnamed namespace
