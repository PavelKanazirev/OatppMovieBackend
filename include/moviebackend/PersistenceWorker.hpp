/**
 * @file PersistenceWorker.hpp
 * @brief Background thread that writes the catalog back to disk after changes.
 *
 * =========================================================================
 *  THE std::condition_variable EXAMPLE IN THIS CODEBASE
 * =========================================================================
 *
 * Catalog needs only a mutex - it has nothing to wait *for*. This class is
 * the place where a condition variable is genuinely the right tool, and it is
 * the textbook shape of the pattern:
 *
 *   * a **producer** side - any request thread that mutated the catalog calls
 *     onCatalogChanged(), which sets a flag and signals;
 *   * a **consumer** side - one writer thread that sleeps until either the
 *     flag is set or shutdown is requested, and otherwise costs nothing.
 *
 * The alternative - a thread that wakes every N milliseconds to poll a bool -
 * either burns CPU while idle or adds latency. The condition variable gives
 * an idle cost of exactly zero and a wake-up latency of microseconds.
 *
 * ## The three things that make a CV correct, all present below
 *
 * 1. **The predicate is protected by the same mutex as the wait.** m_dirty
 *    and m_stopRequested are only ever touched with m_mutex held.
 *
 * 2. **The wait uses the predicate overload**, i.e.
 *    `m_condition.wait(lock, predicate)` rather than a bare `wait(lock)`.
 *    That is what makes spurious wake-ups - which are real and do happen -
 *    a non-issue: the thread re-checks the predicate and goes back to sleep.
 *
 * 3. **The signal happens after the state change**, with the mutex held, so
 *    the waiter cannot miss the notification by testing the flag a moment
 *    before it is set (the lost-wakeup race).
 *
 * ## The debounce
 *
 * Booking ten seats in a burst should produce one file write, not ten. After
 * being woken the thread waits out a short quiet period, coalescing whatever
 * else arrives, and only then serialises. That is done with
 * `wait_for(..., predicate)` on the same condition variable, so a shutdown
 * request still interrupts it immediately instead of being delayed by the
 * debounce.
 */
#ifndef MOVIEBACKEND_PERSISTENCEWORKER_HPP
#define MOVIEBACKEND_PERSISTENCEWORKER_HPP

#include "moviebackend/Catalog.hpp"
#include "moviebackend/CatalogStore.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace moviebackend {

/**
 * @brief Observes the catalog and persists it on a dedicated thread.
 *
 * Lifetime contract, which the constructor and destructor enforce:
 *   * the constructor registers this object as the catalog's change listener
 *     and starts the writer thread;
 *   * the destructor unregisters, asks the thread to stop, joins it, and
 *     performs one final save if anything is still pending.
 *
 * So the Catalog must outlive the worker. main() creates the catalog first,
 * which guarantees exactly that.
 */
class PersistenceWorker : public CatalogChangeListener {
public:
    /**
     * @param catalog    the catalog to observe; must outlive this object
     * @param store      where to write; must outlive this object
     * @param debounce   quiet period collected before a write
     */
    PersistenceWorker(Catalog& catalog,
                      CatalogStore& store,
                      std::chrono::milliseconds debounce);

    /** @brief Stops and joins the writer thread, then flushes if dirty. */
    ~PersistenceWorker() override;

    PersistenceWorker(const PersistenceWorker&) = delete;
    PersistenceWorker& operator=(const PersistenceWorker&) = delete;

    /**
     * @brief CALLBACK - the catalog changed.
     *
     * Called from an arbitrary request thread with no catalog lock held.
     * Marks the state dirty and signals the writer. Returns immediately; the
     * actual file write happens on the worker thread.
     *
     * CRITICAL SECTION: PersistenceWorker::m_mutex, held for two assignments.
     */
    void onCatalogChanged() noexcept override;

    /**
     * @brief Block until every change made so far has been written.
     *
     * Used by the tests and by the clean-shutdown path. Also uses a condition
     * variable - it waits for the writer to report a generation number at
     * least as high as the one pending when the call was made.
     *
     * @param timeout maximum time to wait
     * @return true if everything pending was flushed, false on timeout
     */
    bool waitForIdle(std::chrono::milliseconds timeout);

    /** @return how many times the store has been written since construction. */
    std::uint64_t saveCount() const;

private:
    /** @brief The writer thread body. */
    void run();

    /**
     * @brief Snapshot the catalog and hand it to the store.
     *
     * Called with m_mutex NOT held: taking the catalog snapshot acquires the
     * *catalog's* lock, and file I/O must never happen under either lock.
     *
     * @return true if the store accepted the write, false if it threw. The
     *         caller must not treat a false as progress - see run().
     */
    bool performSave() noexcept;

    Catalog&                  m_catalog;
    CatalogStore&             m_store;
    std::chrono::milliseconds m_debounce;

    /**
     * CRITICAL SECTION: guards m_dirty, m_stopRequested, m_pendingGeneration
     * and m_savedGeneration. Paired with m_condition.
     */
    mutable std::mutex      m_mutex;
    std::condition_variable m_condition;

    /** Set by onCatalogChanged(), cleared by the writer before it saves. */
    bool m_dirty = false;

    /** Set once by the destructor to end the writer loop. */
    bool m_stopRequested = false;

    /** Incremented on every change; lets waitForIdle know what to wait for. */
    std::uint64_t m_pendingGeneration = 0;

    /** The highest generation that has actually reached the store. */
    std::uint64_t m_savedGeneration = 0;

    /** Number of completed writes, for the tests and for logging. */
    std::uint64_t m_saveCount = 0;

    /** Number of writes the store rejected. Reported at shutdown. */
    std::uint64_t m_failedSaveCount = 0;

    /**
     * How long to wait after a failed write before trying again.
     *
     * Without a backoff, a permanently unwritable store (a full or read-only
     * disk) would turn this thread into a busy loop, because the retry path
     * re-arms the dirty flag immediately.
     */
    static constexpr std::chrono::milliseconds kRetryBackoff{250};

    /**
     * The writer thread. Declared **last** so that it is constructed after
     * everything it touches and destroyed first - a plain member-order rule
     * that removes a whole class of start-up races.
     *
     * std::thread rather than C++20's std::jthread: the stop flag and the
     * condition variable are already doing the work a stop_token would do,
     * and one explicit mechanism is easier to follow than two.
     */
    std::thread m_thread;
};

} // namespace moviebackend

#endif // MOVIEBACKEND_PERSISTENCEWORKER_HPP
