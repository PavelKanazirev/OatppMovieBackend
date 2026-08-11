/**
 * @file ScheduleTest.cpp
 * @brief Tests for time parsing and the default-timeline rule.
 *
 * The generation rule is stated in prose in the requirements ("starting from
 * 9:00 a.m. to 21:00 p.m., next one starting at a round hour after the
 * timeline"), so these tests are where that prose is pinned down to something
 * unambiguous.
 */
#include "moviebackend/Schedule.hpp"

#include "moviebackend/Errors.hpp"
#include "TestSupport.hpp"

#include <gtest/gtest.h>

using moviebackend::ErrorCode;
using moviebackend::MinutesSinceMidnight;
using moviebackend::ScheduleConfig;
namespace schedule = moviebackend::schedule;

namespace {

TEST(ScheduleTest, ParsesValidTimes)
{
    EXPECT_EQ(0, schedule::parseTimeOfDay("00:00"));
    EXPECT_EQ(540, schedule::parseTimeOfDay("09:00"));
    EXPECT_EQ(1260, schedule::parseTimeOfDay("21:00"));
    EXPECT_EQ(1439, schedule::parseTimeOfDay("23:59"));
}

TEST(ScheduleTest, RejectsMalformedTimes)
{
    EXPECT_SERVICE_ERROR(schedule::parseTimeOfDay(""), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(schedule::parseTimeOfDay("9:00"), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(schedule::parseTimeOfDay("09-00"), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(schedule::parseTimeOfDay("aa:bb"), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(schedule::parseTimeOfDay("24:00"), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(schedule::parseTimeOfDay("09:60"), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(schedule::parseTimeOfDay("09:000"), ErrorCode::InvalidArgument);
}

TEST(ScheduleTest, FormatsWithZeroPadding)
{
    EXPECT_EQ("00:00", schedule::formatTimeOfDay(0));
    EXPECT_EQ("09:05", schedule::formatTimeOfDay(9 * 60 + 5));
    EXPECT_EQ("21:00", schedule::formatTimeOfDay(21 * 60));
    EXPECT_EQ("23:59", schedule::formatTimeOfDay(1439));
}

TEST(ScheduleTest, FormatRejectsOutOfRange)
{
    EXPECT_SERVICE_ERROR(schedule::formatTimeOfDay(-1), ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(schedule::formatTimeOfDay(1440), ErrorCode::InvalidArgument);
}

TEST(ScheduleTest, ParseAndFormatRoundTrip)
{
    for (MinutesSinceMidnight minutes = 0; minutes < 1440; ++minutes) {
        const std::string text = schedule::formatTimeOfDay(minutes);
        EXPECT_EQ(minutes, schedule::parseTimeOfDay(text)) << "at '" << text << "'";
    }
}

TEST(ScheduleTest, RoundUpLeavesWholeHoursAlone)
{
    // The boundary decision documented in Schedule.hpp: a time already on the
    // hour does not get pushed to the next one.
    EXPECT_EQ(11 * 60, schedule::roundUpToWholeHour(11 * 60));
    EXPECT_EQ(12 * 60, schedule::roundUpToWholeHour(11 * 60 + 1));
    EXPECT_EQ(11 * 60, schedule::roundUpToWholeHour(10 * 60 + 50));
}

TEST(ScheduleTest, DefaultScheduleForA110MinuteFilm)
{
    // The worked example from the header, and the canonical case for the
    // requirement.
    ScheduleConfig config;   // 09:00 .. 21:00

    const std::vector<MinutesSinceMidnight> starts =
        schedule::generateDefaultStartTimes(110, config);

    const std::vector<std::string> expected = {
        "09:00", "11:00", "13:00", "15:00", "17:00", "19:00", "21:00"
    };

    ASSERT_EQ(expected.size(), starts.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], schedule::formatTimeOfDay(starts[i])) << "slot " << i;
    }
}

TEST(ScheduleTest, FilmEndingExactlyOnTheHourGetsABackToBackSlot)
{
    // A 120 minute film starting at 09:00 ends at 11:00, so the next slot is
    // 11:00 - not 12:00. This is the "at or after" reading, and it is the
    // difference between 7 screenings and 4.
    ScheduleConfig config;

    const std::vector<MinutesSinceMidnight> starts =
        schedule::generateDefaultStartTimes(120, config);

    ASSERT_GE(starts.size(), 2u);
    EXPECT_EQ("09:00", schedule::formatTimeOfDay(starts[0]));
    EXPECT_EQ("11:00", schedule::formatTimeOfDay(starts[1]));
    EXPECT_EQ("21:00", schedule::formatTimeOfDay(starts.back()));
    EXPECT_EQ(7u, starts.size());
}

TEST(ScheduleTest, ShortFilmFillsEveryHour)
{
    ScheduleConfig config;

    const std::vector<MinutesSinceMidnight> starts =
        schedule::generateDefaultStartTimes(45, config);

    // 09:00 through 21:00 inclusive, one per hour.
    ASSERT_EQ(13u, starts.size());
    EXPECT_EQ("09:00", schedule::formatTimeOfDay(starts.front()));
    EXPECT_EQ("21:00", schedule::formatTimeOfDay(starts.back()));
}

TEST(ScheduleTest, VeryLongFilmGetsASingleScreening)
{
    ScheduleConfig config;

    const std::vector<MinutesSinceMidnight> starts =
        schedule::generateDefaultStartTimes(13 * 60, config);

    ASSERT_EQ(1u, starts.size());
    EXPECT_EQ("09:00", schedule::formatTimeOfDay(starts[0]));
}

TEST(ScheduleTest, DoesNotWrapPastMidnight)
{
    // A film that would push the next slot past midnight must stop rather
    // than wrap around to the small hours.
    ScheduleConfig config;
    config.dayStart = 22 * 60;
    config.dayEnd = 23 * 60;

    const std::vector<MinutesSinceMidnight> starts =
        schedule::generateDefaultStartTimes(180, config);

    ASSERT_EQ(1u, starts.size());
    EXPECT_EQ("22:00", schedule::formatTimeOfDay(starts[0]));
}

TEST(ScheduleTest, HonoursCustomDayBounds)
{
    ScheduleConfig config;
    config.dayStart = 12 * 60;
    config.dayEnd = 15 * 60;

    const std::vector<MinutesSinceMidnight> starts =
        schedule::generateDefaultStartTimes(90, config);

    ASSERT_EQ(2u, starts.size());
    EXPECT_EQ("12:00", schedule::formatTimeOfDay(starts[0]));
    EXPECT_EQ("14:00", schedule::formatTimeOfDay(starts[1]));
}

TEST(ScheduleTest, RejectsInvalidInputs)
{
    ScheduleConfig config;

    EXPECT_SERVICE_ERROR(schedule::generateDefaultStartTimes(0, config),
                         ErrorCode::InvalidArgument);
    EXPECT_SERVICE_ERROR(schedule::generateDefaultStartTimes(-10, config),
                         ErrorCode::InvalidArgument);

    ScheduleConfig inverted;
    inverted.dayStart = 21 * 60;
    inverted.dayEnd = 9 * 60;
    EXPECT_SERVICE_ERROR(schedule::generateDefaultStartTimes(90, inverted),
                         ErrorCode::InvalidArgument);
}

} // unnamed namespace
