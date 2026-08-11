/**
 * @file Schedule.hpp
 * @brief Time-of-day parsing/formatting and default showtime generation.
 *
 * These are free functions in a namespace rather than static members of a
 * class: they hold no state, and a class would only be scaffolding.
 */
#ifndef MOVIEBACKEND_SCHEDULE_HPP
#define MOVIEBACKEND_SCHEDULE_HPP

#include "moviebackend/Domain.hpp"

#include <string>
#include <vector>

namespace moviebackend {

/** @brief Bounds of the screening day, read from config.json. */
struct ScheduleConfig {
    /** Earliest permitted start time. Default 09:00. */
    MinutesSinceMidnight dayStart = 9 * 60;

    /** Latest permitted *start* time (a film may run past it). Default 21:00. */
    MinutesSinceMidnight dayEnd = 21 * 60;
};

namespace schedule {

/**
 * @brief Parse "HH:MM" into minutes since midnight.
 * @param text a 5-character 24-hour time, e.g. "09:00" or "21:30"
 * @return minutes since midnight, 0..1439
 * @throws ServiceError InvalidArgument if the text is not a valid time.
 */
MinutesSinceMidnight parseTimeOfDay(const std::string& text);

/**
 * @brief Format minutes since midnight as "HH:MM".
 * @param minutes 0..1439
 * @throws ServiceError InvalidArgument if out of range.
 */
std::string formatTimeOfDay(MinutesSinceMidnight minutes);

/**
 * @brief Round a time up to the next whole hour.
 *
 * A time that is already exactly on the hour is returned unchanged - 11:00
 * rounds to 11:00, 11:01 rounds to 12:00.
 *
 * @param minutes any time of day
 * @return the smallest whole hour >= @p minutes
 */
MinutesSinceMidnight roundUpToWholeHour(MinutesSinceMidnight minutes);

/**
 * @brief Generate the default set of start times for a movie in a theater.
 *
 * The rule from the requirements, spelled out:
 *
 *   * the first screening starts at `config.dayStart` (09:00 by default);
 *   * each following screening starts at the first whole hour at or after
 *     the previous screening *ends*;
 *   * screenings are emitted while the start time is <= `config.dayEnd`
 *     (21:00 by default). A film is allowed to run past dayEnd; it is only
 *     the start time that is capped.
 *
 * Worked example - a 110 minute film, 09:00..21:00:
 *
 *     09:00 (ends 10:50) -> next whole hour 11:00
 *     11:00 (ends 12:50) -> 13:00
 *     13:00 -> 15:00 -> 17:00 -> 19:00 -> 21:00   [stop, next would be 23:00]
 *
 * Boundary decision worth knowing about: a film ending *exactly* on the hour
 * (a 120 minute film starting at 09:00 ends at 11:00) yields 11:00 as the
 * next slot, not 12:00 - "a round hour after the timeline" is read as
 * "at or after", giving back-to-back screenings with no idle hour.
 *
 * @param movieDurationMinutes running time, must be > 0
 * @param config               day bounds
 * @return the start times, ascending; never empty for a valid input
 * @throws ServiceError InvalidArgument if the duration is <= 0 or the day
 *         bounds are inverted.
 */
std::vector<MinutesSinceMidnight> generateDefaultStartTimes(
    int movieDurationMinutes,
    const ScheduleConfig& config);

} // namespace schedule
} // namespace moviebackend

#endif // MOVIEBACKEND_SCHEDULE_HPP
