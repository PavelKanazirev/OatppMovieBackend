/**
 * @file PersistenceWorkerTest.cpp
 * @brief Tests for the background writer thread.
 *
 * These tests are all about *timing*, which makes them the easiest ones in
 * the suite to write in a flaky way. The rule followed here: never assert
 * "this happened within N milliseconds". Instead, drive the worker and then
 * block on waitForIdle(), which is a condition-variable wait rather than a
 * sleep. The only timeouts present are generous upper bounds whose expiry
 * means a genuine failure (a deadlock), not a slow machine.
 */
#include "moviebackend/PersistenceWorker.hpp"

#include "moviebackend/Catalog.hpp"
#include "TestSupport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

using moviebackend::Catalog;
using moviebackend::CatalogSnapshot;
using moviebackend::PersistenceWorker;
using moviebackend::Showtime;
using moviebackend::TheaterDefaults;
using moviebackend::testsupport::InMemoryCatalogStore;
using moviebackend::testsupport::makeMovie;
using moviebackend::testsupport::makeTheater;

namespace {

/** Generous - its expiry means a hang, not a slow machine. */
constexpr std::chrono::milliseconds kFlushTimeout(10000);

class PersistenceWorkerTest : public ::testing::Test {
protected:
    PersistenceWorkerTest()
        : m_defaults()
        , m_catalog(m_defaults)
        , m_store()
    {
    }

    void seedCatalog()
    {
        m_catalog.addMovie(makeMovie("m1", "Persisted", 100));
        m_catalog.addTheater(makeTheater("t1", "Hall", 20, 10));

        Showtime showtime;
        showtime.id = "s1";
        showtime.movieId = "m1";
        showtime.theaterId = "t1";
        showtime.startTime = 9 * 60;
        m_catalog.addShowtime(showtime);
    }

    TheaterDefaults       m_defaults;
    Catalog               m_catalog;
    InMemoryCatalogStore  m_store;
};

TEST_F(PersistenceWorkerTest, ChangesReachTheStoreWithoutAnyPolling)
{
    PersistenceWorker worker(m_catalog, m_store, std::chrono::milliseconds(0));

    m_catalog.addMovie(makeMovie("m1", "Written", 100));

    ASSERT_TRUE(worker.waitForIdle(kFlushTimeout)) << "the worker never caught up";

    const CatalogSnapshot stored = m_store.stored();
    ASSERT_EQ(1u, stored.movies.size());
    EXPECT_EQ("Written", stored.movies[0].title);
}

TEST_F(PersistenceWorkerTest, ABurstOfChangesIsCoalescedIntoFewerWrites)
{
    // The point of the debounce. 50 changes must not produce 50 files.
    PersistenceWorker worker(m_catalog, m_store, std::chrono::milliseconds(50));

    for (int i = 0; i < 50; ++i) {
        m_catalog.addMovie(makeMovie("", "Movie " + std::to_string(i), 100));
    }

    ASSERT_TRUE(worker.waitForIdle(kFlushTimeout));

    // Every change must be present...
    EXPECT_EQ(50u, m_store.stored().movies.size());

    // ...but not one write per change. The exact number depends on timing, so
    // the assertion is a loose upper bound rather than an exact count - which
    // is the difference between a meaningful test and a flaky one.
    EXPECT_LT(m_store.saveCount(), 25u)
        << "the debounce did not coalesce anything";
    EXPECT_GT(m_store.saveCount(), 0u);
}

TEST_F(PersistenceWorkerTest, BookingsArePersisted)
{
    seedCatalog();

    PersistenceWorker worker(m_catalog, m_store, std::chrono::milliseconds(0));

    m_catalog.bookSeats("s1", {"a1", "a2"}, "Alice");

    ASSERT_TRUE(worker.waitForIdle(kFlushTimeout));

    const CatalogSnapshot stored = m_store.stored();
    ASSERT_EQ(1u, stored.showtimes.size());
    EXPECT_EQ(2u, stored.showtimes[0].bookedSeats.size());
    ASSERT_EQ(1u, stored.bookings.size());
    EXPECT_EQ("Alice", stored.bookings[0].customerName);
}

TEST_F(PersistenceWorkerTest, ShutdownFlushesAnythingStillPending)
{
    // A long debounce guarantees the change is still sitting in the worker
    // when the destructor runs, which is exactly the case the final flush in
    // ~PersistenceWorker() exists for.
    {
        PersistenceWorker worker(m_catalog, m_store,
                                 std::chrono::milliseconds(60000));
        m_catalog.addMovie(makeMovie("m1", "Flushed On Exit", 100));
    } // destructor runs here

    const CatalogSnapshot stored = m_store.stored();
    ASSERT_EQ(1u, stored.movies.size());
    EXPECT_EQ("Flushed On Exit", stored.movies[0].title);
}

TEST_F(PersistenceWorkerTest, DestructionWithNoChangesWritesNothing)
{
    {
        PersistenceWorker worker(m_catalog, m_store, std::chrono::milliseconds(0));
    }
    EXPECT_EQ(0u, m_store.saveCount());
}

TEST_F(PersistenceWorkerTest, AFailingStoreDoesNotBringDownTheService)
{
    // performSave() is noexcept; a disk error must be logged and swallowed,
    // not allowed to terminate the process from a background thread.
    PersistenceWorker worker(m_catalog, m_store, std::chrono::milliseconds(0));

    m_store.failNextSave();
    m_catalog.addMovie(makeMovie("m1", "First", 100));
    ASSERT_TRUE(worker.waitForIdle(kFlushTimeout));

    // The worker is still alive and still saves.
    m_catalog.addMovie(makeMovie("m2", "Second", 100));
    ASSERT_TRUE(worker.waitForIdle(kFlushTimeout));

    EXPECT_EQ(2u, m_store.stored().movies.size());
}

TEST_F(PersistenceWorkerTest, AFailedSaveIsNotReportedAsProgressAndIsRetried)
{
    // Regression guard for silent data loss.
    //
    // The worker clears its dirty flag *before* saving, so that a change
    // arriving mid-write is not swallowed. The bug was that it then advanced
    // the saved-generation counter unconditionally - so a save that threw was
    // indistinguishable from one that succeeded. On a full or read-only disk
    // every booking since the last good write was lost, waitForIdle() returned
    // true, and the shutdown log said everything was fine.
    PersistenceWorker worker(m_catalog, m_store, std::chrono::milliseconds(0));

    m_store.failAllSaves(true);
    m_catalog.addMovie(makeMovie("m1", "Never Written", 100));

    // While the store is failing, the worker must NOT claim to be caught up.
    EXPECT_FALSE(worker.waitForIdle(std::chrono::milliseconds(400)))
        << "waitForIdle() reported success for a write that never happened";
    EXPECT_EQ(0u, m_store.saveCount());

    // Once the store recovers, the pending change must still be retried -
    // it was re-armed rather than dropped.
    m_store.failAllSaves(false);

    ASSERT_TRUE(worker.waitForIdle(kFlushTimeout))
        << "the worker never retried after the store recovered";

    const CatalogSnapshot stored = m_store.stored();
    ASSERT_EQ(1u, stored.movies.size());
    EXPECT_EQ("Never Written", stored.movies[0].title);
}

TEST_F(PersistenceWorkerTest, APermanentlyFailingStoreDoesNotSpinOrHangShutdown)
{
    // The retry must be paced, and a stop request must still win immediately.
    m_store.failAllSaves(true);

    {
        PersistenceWorker worker(m_catalog, m_store, std::chrono::milliseconds(0));
        m_catalog.addMovie(makeMovie("m1", "Doomed", 100));
        EXPECT_FALSE(worker.waitForIdle(std::chrono::milliseconds(600)));
    } // the destructor must return promptly despite the failing store

    EXPECT_EQ(0u, m_store.saveCount());
}

TEST_F(PersistenceWorkerTest, UnregistersItselfSoTheCatalogCanOutliveIt)
{
    // The lifetime contract from the header: the catalog must be safe to use
    // after the worker is gone. If the listener pointer were left dangling,
    // this would be a use-after-free - which is why the memcheck target runs
    // the unit tests.
    {
        PersistenceWorker worker(m_catalog, m_store, std::chrono::milliseconds(0));
        m_catalog.addMovie(makeMovie("m1", "Before", 100));
        ASSERT_TRUE(worker.waitForIdle(kFlushTimeout));
    }

    EXPECT_NO_THROW(m_catalog.addMovie(makeMovie("m2", "After", 100)));

    // The second movie was added after the worker died, so it must not have
    // reached the store.
    EXPECT_EQ(1u, m_store.stored().movies.size());
}

TEST_F(PersistenceWorkerTest, ConcurrentBookingsAllReachTheStore)
{
    seedCatalog();

    PersistenceWorker worker(m_catalog, m_store, std::chrono::milliseconds(10));

    const int threadCount = 8;
    std::vector<std::thread> threads;

    for (int t = 0; t < threadCount; ++t) {
        const std::string seat = "a" + std::to_string(t + 1);
        threads.push_back(std::thread([this, seat]() {
            try {
                m_catalog.bookSeats("s1", {seat}, "racer");
            } catch (const moviebackend::ServiceError&) {
                // seat contention is fine here
            }
        }));
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    ASSERT_TRUE(worker.waitForIdle(kFlushTimeout));

    // Whatever was booked in memory must be what is in the store.
    const CatalogSnapshot stored = m_store.stored();
    ASSERT_EQ(1u, stored.showtimes.size());
    EXPECT_EQ(m_catalog.listBookedSeats("s1").size(),
              stored.showtimes[0].bookedSeats.size());
    EXPECT_EQ(m_catalog.listBookings().size(), stored.bookings.size());
}

} // unnamed namespace
