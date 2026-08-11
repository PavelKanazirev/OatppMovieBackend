/**
 * @file Schedule.cpp
 * @brief Time-of-day handling and default timeline generation.
 */
#include "moviebackend/Schedule.hpp"

#include "moviebackend/Errors.hpp"

#include <string>

namespace moviebackend {
namespace schedule {

namespace {

/** Minutes in a day; the upper bound for any time-of-day value. */
constexpr int kMinutesPerDay = 24 * 60;

/** Minutes in an hour. */
constexpr int kMinutesPerHour = 60;

/**
 * @brief Parse exactly two ASCII digits at @p offset.
 * @return the value, or -1 if the characters are not both digits.
 */
int parseTwoDigits(const std::string& text, std::size_t offset)
{
    const char high = text[offset];
    const char low  = text[offset + 1];

    if (high < '0' || high > '9' || low < '0' || low > '9') {
        return -1;
    }
    return (high - '0') * 10 + (low - '0');
}

} // unnamed namespace

MinutesSinceMidnight parseTimeOfDay(const std::string& text)
{
    // Strict "HH:MM". Being strict here means every other layer can trust the
    // value it receives, which is worth more than accepting "9:00".
    if (text.size() != 5 || text[2] != ':') {
        throw invalidArgument("time must be formatted as HH:MM, got '" + text + "'");
    }

    const int hours   = parseTwoDigits(text, 0);
    const int minutes = parseTwoDigits(text, 3);

    if (hours < 0 || minutes < 0) {
        throw invalidArgument("time must be formatted as HH:MM, got '" + text + "'");
    }
    if (hours > 23) {
        throw invalidArgument("hour must be 00..23, got '" + text + "'");
    }
    if (minutes > 59) {
        throw invalidArgument("minute must be 00..59, got '" + text + "'");
    }

    return hours * kMinutesPerHour + minutes;
}

std::string formatTimeOfDay(MinutesSinceMidnight minutes)
{
    if (minutes < 0 || minutes >= kMinutesPerDay) {
        throw invalidArgument("time of day out of range: "
                              + std::to_string(minutes) + " minutes");
    }

    const int hours = minutes / kMinutesPerHour;
    const int mins  = minutes % kMinutesPerHour;

    // Explicit two-character zero padding. std::format would be the modern
    // one-liner here, but plain arithmetic keeps this readable and avoids
    // depending on a library feature for something this small.
    std::string result;
    result.reserve(5);
    result.push_back(static_cast<char>('0' + hours / 10));
    result.push_back(static_cast<char>('0' + hours % 10));
    result.push_back(':');
    result.push_back(static_cast<char>('0' + mins / 10));
    result.push_back(static_cast<char>('0' + mins % 10));
    return result;
}

MinutesSinceMidnight roundUpToWholeHour(MinutesSinceMidnight minutes)
{
    const int remainder = minutes % kMinutesPerHour;
    if (remainder == 0) {
        return minutes; // already on the hour - see the note in the header
    }
    return minutes + (kMinutesPerHour - remainder);
}

std::vector<MinutesSinceMidnight> generateDefaultStartTimes(
    int movieDurationMinutes,
    const ScheduleConfig& config)
{
    if (movieDurationMinutes <= 0) {
        throw invalidArgument("movie duration must be positive, got "
                              + std::to_string(movieDurationMinutes));
    }
    if (config.dayStart < 0 || config.dayStart >= kMinutesPerDay) {
        throw invalidArgument("schedule day_start is outside the day");
    }
    if (config.dayEnd < config.dayStart) {
        throw invalidArgument("schedule day_end must not be before day_start");
    }

    std::vector<MinutesSinceMidnight> startTimes;

    // The rule, restated: start at dayStart; the next screening begins at the
    // first whole hour at or after the previous one *ends*; keep going while
    // the start time is still within the day window.
    MinutesSinceMidnight current = config.dayStart;

    while (current <= config.dayEnd) {
        startTimes.push_back(current);

        const MinutesSinceMidnight endOfScreening = current + movieDurationMinutes;
        const MinutesSinceMidnight nextStart = roundUpToWholeHour(endOfScreening);

        // A film longer than a day, or one that pushes past midnight, must not
        // wrap around into the next morning.
        if (nextStart >= kMinutesPerDay || nextStart <= current) {
            break;
        }
        current = nextStart;
    }

    return startTimes;
}

} // namespace schedule
} // namespace moviebackend
