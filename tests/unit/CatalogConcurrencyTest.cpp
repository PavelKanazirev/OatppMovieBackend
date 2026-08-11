/**
 * @file CatalogConcurrencyTest.cpp
 * @brief The over-booking tests. This is the core requirement of the exercise.
 *
 * ## What these tests can and cannot prove
 *
 * A race condition is not deterministic, so no test can *prove* its absence.
 * What these tests do is make a race overwhelmingly likely to show up if the
 * locking were wrong, and then assert an invariant that a race would violate.
 *
 * The technique used throughout is a **starting gate**: every worker thread is
 * created, then blocks on a condition variable until the test thread opens the
 * gate. Without it, thread 1 would typically finish before thread 8 was even
 * spawned and the test would pass on a completely broken implementation.
 * With it, all N threads hit the catalog within microseconds of each other.
 *
 * The invariant asserted is always the same and is exactly the requirement:
 *
 *     successful bookings == seats actually taken == seats no longer available
 *
 * If two threads both got seat "a1", that equality breaks.
 *
 * These tests are also the ones worth running under ThreadSanitizer and
 * Valgrind's memcheck - see the README.
 */
#include "moviebackend/Catalog.hpp"

#include "moviebackend/Errors.hpp"
#include "TestSupport.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using moviebackend::Booking;
using moviebackend::Catalog;
using moviebackend::ErrorCode;
using moviebackend::SeatLabel;
using moviebackend::ServiceError;
using moviebackend::Showtime;
using moviebackend::TheaterDefaults;
using moviebackend::testsupport::makeMovie;
using moviebackend::testsupport::makeTheater;

namespace {

/**
 * @brief Holds every worker thread until the test says go.
 *
 * mutex + condition_variable, the same pattern the production code uses in
 * PersistenceWorker - deliberately, so there is one concurrency idiom to
 * learn rather than two.
 */
class StartingGate {
public:
    /** @brief Called by a worker: block until open() is called. */
    void waitForStart()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock, [this]() { return m_open; });
    }

    /** @brief Called by the test: release every waiting worker at once. */
    void open()
    {
        {
            const std::lock_guard<std::mutex> guard(m_mutex);
            m_open = true;
        }
        m_condition.notify_all();
    }

private:
    std::mutex              m_mutex;
    std::condition_variable m_condition;
    bool                    m_open = false;
};

/** @brief Outcome of one worker's booking attempts. */
struct WorkerResult {
    std::vector<SeatLabel> seatsWon;
    int successCount = 0;
    int seatUnavailableCount = 0;
    int unexpectedErrorCount = 0;
    std::string unexpectedMessage;
};

/**
 * @brief Build a catalog with one showtime of @p capacity seats.
 */
void seedSingleShowtime(Catalog& catalog, int capacity, int seatsPerRow)
{
    catalog.addMovie(makeMovie("m1", "Contended", 100));
    catalog.addTheater(makeTheater("t1", "The Only Hall", capacity, seatsPerRow));

    Showtime showtime;
    showtime.id = "s1";
    showtime.movieId = "m1";
    showtime.theaterId = "t1";
    showtime.startTime = 9 * 60;
    catalog.addShowtime(showtime);
}

/* =========================================================================
 * Test 1 - many threads, one seat
 * ========================================================================= */

TEST(CatalogConcurrencyTest, OnlyOneThreadCanWinASingleSeat)
{
    // The sharpest possible version of the requirement: 16 threads, 1 seat.
    // Exactly one must succeed; the other 15 must be told SeatUnavailable.
    TheaterDefaults defaults;
    Catalog catalog(defaults);
    seedSingleShowtime(catalog, 20, 10);

    const int threadCount = 16;

    StartingGate gate;
    std::atomic<int> successes(0);
    std::atomic<int> rejections(0);
    std::atomic<int> unexpected(0);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(threadCount));

    for (int i = 0; i < threadCount; ++i) {
        threads.push_back(std::thread([&catalog, &gate, &successes,
                                       &rejections, &unexpected]() {
            gate.waitForStart();
            try {
                catalog.bookSeats("s1", {"a1"}, "contender");
                ++successes;
            } catch (const ServiceError& error) {
                if (error.code() == ErrorCode::SeatUnavailable) {
                    ++rejections;
                } else {
                    ++unexpected;
                }
            }
        }));
    }

    gate.open();
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    EXPECT_EQ(1, successes.load()) << "seat a1 was sold more than once";
    EXPECT_EQ(threadCount - 1, rejections.load());
    EXPECT_EQ(0, unexpected.load());

    EXPECT_EQ(19u, catalog.listAvailableSeats("s1").size());
    EXPECT_EQ(1u, catalog.listBookings().size());
}

/* =========================================================================
 * Test 2 - every thread races for every seat
 * ========================================================================= */

TEST(CatalogConcurrencyTest, NoSeatIsEverSoldTwice)
{
    // 8 threads each try to book all 20 seats one at a time. Between them
    // they will make 160 attempts for 20 seats. Whatever the interleaving,
    // each seat must be won exactly once.
    TheaterDefaults defaults;
    Catalog catalog(defaults);

    const int capacity = 20;
    const int seatsPerRow = 10;
    const int threadCount = 8;

    seedSingleShowtime(catalog, capacity, seatsPerRow);

    const std::vector<SeatLabel> allSeats = catalog.listAvailableSeats("s1");
    ASSERT_EQ(static_cast<std::size_t>(capacity), allSeats.size());

    StartingGate gate;
    std::vector<WorkerResult> results(static_cast<std::size_t>(threadCount));
    std::vector<std::thread> threads;

    for (int t = 0; t < threadCount; ++t) {
        WorkerResult* result = &results[static_cast<std::size_t>(t)];

        threads.push_back(std::thread([&catalog, &gate, &allSeats, result]() {
            gate.waitForStart();

            for (std::size_t i = 0; i < allSeats.size(); ++i) {
                try {
                    const Booking booking =
                        catalog.bookSeats("s1", {allSeats[i]}, "racer");
                    ++result->successCount;
                    result->seatsWon.push_back(booking.seats.front());
                } catch (const ServiceError& error) {
                    if (error.code() == ErrorCode::SeatUnavailable) {
                        ++result->seatUnavailableCount;
                    } else {
                        ++result->unexpectedErrorCount;
                        result->unexpectedMessage = error.what();
                    }
                }
            }
        }));
    }

    gate.open();
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    // --- The invariant ---------------------------------------------------
    std::set<SeatLabel> wonSeats;
    int totalSuccesses = 0;

    for (std::size_t t = 0; t < results.size(); ++t) {
        EXPECT_EQ(0, results[t].unexpectedErrorCount)
            << "unexpected error: " << results[t].unexpectedMessage;

        totalSuccesses += results[t].successCount;

        for (std::size_t i = 0; i < results[t].seatsWon.size(); ++i) {
            EXPECT_TRUE(wonSeats.insert(results[t].seatsWon[i]).second)
                << "seat " << results[t].seatsWon[i] << " was sold twice";
        }
    }

    EXPECT_EQ(capacity, totalSuccesses)
        << "every seat should be sold exactly once";
    EXPECT_EQ(static_cast<std::size_t>(capacity), wonSeats.size());

    // And the catalog agrees.
    EXPECT_TRUE(catalog.listAvailableSeats("s1").empty());
    EXPECT_EQ(static_cast<std::size_t>(capacity), catalog.listBookings().size());
}

/* =========================================================================
 * Test 3 - multi-seat requests must stay all-or-nothing under contention
 * ========================================================================= */

TEST(CatalogConcurrencyTest, MultiSeatBookingsAreAtomicUnderContention)
{
    // 12 threads each ask for the same overlapping pair of seats. A booking
    // that succeeded must own *both* seats; a booking that failed must own
    // neither. A torn, half-applied booking would show up as a seat count
    // that is not a multiple of two.
    TheaterDefaults defaults;
    Catalog catalog(defaults);
    seedSingleShowtime(catalog, 20, 10);

    const int threadCount = 12;

    StartingGate gate;
    std::atomic<int> successes(0);
    std::atomic<int> unexpected(0);

    std::vector<std::thread> threads;

    for (int t = 0; t < threadCount; ++t) {
        // Threads alternate between two overlapping pairs so that some
        // requests conflict on only one of their two seats - the case where a
        // non-atomic implementation would leak a partial reservation.
        const bool useOverlapping = (t % 2) == 0;

        threads.push_back(std::thread([&catalog, &gate, &successes,
                                       &unexpected, useOverlapping]() {
            const std::vector<SeatLabel> wanted =
                useOverlapping ? std::vector<SeatLabel>{"a1", "a2"}
                               : std::vector<SeatLabel>{"a2", "a3"};

            gate.waitForStart();
            try {
                catalog.bookSeats("s1", wanted, "pair-racer");
                ++successes;
            } catch (const ServiceError& error) {
                if (error.code() != ErrorCode::SeatUnavailable) {
                    ++unexpected;
                }
            }
        }));
    }

    gate.open();
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    EXPECT_EQ(0, unexpected.load());

    // Both pairs contain a2, so exactly one request can ever win.
    EXPECT_EQ(1, successes.load());

    const std::vector<Booking> bookings = catalog.listBookings();
    ASSERT_EQ(1u, bookings.size());
    EXPECT_EQ(2u, bookings.front().seats.size());

    // 20 seats, one booking of two: exactly 18 free, and a3 or a1 - whichever
    // was not part of the winning pair - must still be free.
    EXPECT_EQ(18u, catalog.listAvailableSeats("s1").size());
}

/* =========================================================================
 * Test 4 - readers and writers together
 * ========================================================================= */

TEST(CatalogConcurrencyTest, ConcurrentReadsNeverObserveATornState)
{
    // Readers run while bookings are happening.
    //
    // The invariant has to be checked from a *single* locked read, which is
    // why this uses snapshot() rather than calling listAvailableSeats() and
    // listBookedSeats() one after the other. Two separate calls take the lock
    // twice, and a booking landing in between changes the answer legitimately
    // - that is a stale read, not a torn one, and asserting on it would be
    // testing the test rather than the catalog.
    //
    // What snapshot() gives us is one consistent view, in which two facts
    // must always agree: the seats marked booked in the showtime, and the
    // seats claimed by the booking records. bookSeats() writes both inside
    // one critical section, so a reader that ever saw them disagree would
    // have caught the mutation half-applied.
    TheaterDefaults defaults;
    Catalog catalog(defaults);

    const int capacity = 20;
    seedSingleShowtime(catalog, capacity, 10);

    const std::vector<SeatLabel> allSeats = catalog.listAvailableSeats("s1");

    StartingGate gate;
    std::atomic<bool> bookingDone(false);
    std::atomic<int> inconsistentReads(0);
    std::atomic<int> readCount(0);

    // One writer books every seat, one at a time.
    std::thread writer([&catalog, &gate, &allSeats, &bookingDone]() {
        gate.waitForStart();
        for (std::size_t i = 0; i < allSeats.size(); ++i) {
            try {
                catalog.bookSeats("s1", {allSeats[i]}, "writer");
            } catch (const ServiceError&) {
                // not expected here, but a failure must not abort the loop
            }
        }
        bookingDone = true;
    });

    // Several readers check the invariant continuously.
    //
    // The loop condition carries a minimum read count as well as the done
    // flag. Without it the test is flaky under a serialising scheduler - most
    // visibly under Valgrind, which effectively pins every thread to one core:
    // the writer can complete all twenty bookings before a reader is ever
    // scheduled, leaving readCount at zero and failing an assertion for a
    // reason that has nothing to do with the catalog. Guaranteeing a few reads
    // regardless keeps the invariant check meaningful in both worlds.
    const int minimumReadsPerThread = 5;

    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.push_back(std::thread([&catalog, &gate, &bookingDone,
                                       &inconsistentReads, &readCount,
                                       capacity, minimumReadsPerThread]() {
            gate.waitForStart();

            int localReads = 0;
            while (!bookingDone.load() || localReads < minimumReadsPerThread) {
                const moviebackend::CatalogSnapshot snapshot = catalog.snapshot();
                ++readCount;
                ++localReads;

                if (snapshot.showtimes.size() != 1u) {
                    ++inconsistentReads;
                    continue;
                }

                const std::size_t markedBooked =
                    snapshot.showtimes[0].bookedSeats.size();

                std::size_t claimedByBookings = 0;
                for (std::size_t b = 0; b < snapshot.bookings.size(); ++b) {
                    if (snapshot.bookings[b].showtimeId == "s1") {
                        claimedByBookings += snapshot.bookings[b].seats.size();
                    }
                }

                // The seat map and the booking ledger must never disagree,
                // and neither may exceed the capacity.
                if (markedBooked != claimedByBookings
                    || markedBooked > static_cast<std::size_t>(capacity)) {
                    ++inconsistentReads;
                }
            }
        }));
    }

    gate.open();
    writer.join();
    for (std::size_t i = 0; i < readers.size(); ++i) {
        readers[i].join();
    }

    EXPECT_EQ(0, inconsistentReads.load())
        << "a reader observed a torn catalog state";
    EXPECT_GT(readCount.load(), 0) << "the readers never actually ran";
    EXPECT_TRUE(catalog.listAvailableSeats("s1").empty());
}

/* =========================================================================
 * Test 5 - mutating admin operations racing with bookings
 * ========================================================================= */

TEST(CatalogConcurrencyTest, AdminChangesRacingWithBookingsStayConsistent)
{
    // An administrator adding and removing showtimes while customers book.
    // Nothing here asserts a particular outcome - showtimes come and go, so
    // NotFound is a legitimate result. What is asserted is that the process
    // does not corrupt itself: no crash, no unexpected error code, and the
    // final state still satisfies free + booked == capacity everywhere.
    TheaterDefaults defaults;
    Catalog catalog(defaults);
    seedSingleShowtime(catalog, 20, 10);

    StartingGate gate;
    std::atomic<bool> stop(false);
    std::atomic<int> unexpected(0);

    std::thread booker([&catalog, &gate, &stop, &unexpected]() {
        gate.waitForStart();
        const std::vector<SeatLabel> seats = {"a1", "a2", "a3", "a4", "a5"};
        std::size_t i = 0;
        while (!stop.load()) {
            try {
                catalog.bookSeats("s1", {seats[i % seats.size()]}, "customer");
            } catch (const ServiceError& error) {
                if (error.code() != ErrorCode::SeatUnavailable
                    && error.code() != ErrorCode::NotFound) {
                    ++unexpected;
                }
            }
            ++i;
        }
    });

    std::thread admin([&catalog, &gate, &unexpected]() {
        gate.waitForStart();
        for (int i = 0; i < 200; ++i) {
            try {
                Showtime extra;
                extra.movieId = "m1";
                extra.theaterId = "t1";
                extra.startTime = 12 * 60;
                const Showtime added = catalog.addShowtime(extra);
                catalog.removeShowtime(added.id);
            } catch (const ServiceError& error) {
                if (error.code() != ErrorCode::Conflict
                    && error.code() != ErrorCode::NotFound) {
                    ++unexpected;
                }
            }
        }
    });

    gate.open();
    admin.join();
    stop = true;
    booker.join();

    EXPECT_EQ(0, unexpected.load());

    const std::size_t free = catalog.listAvailableSeats("s1").size();
    const std::size_t booked = catalog.listBookedSeats("s1").size();
    EXPECT_EQ(20u, free + booked);
}

} // unnamed namespace
