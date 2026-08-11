/**
 * @file PersistenceWorker.cpp
 * @brief The producer/consumer writer thread.
 *
 * The header explains why a condition variable belongs here and what makes
 * the pattern correct. This file is the implementation of exactly that.
 */
#include "moviebackend/PersistenceWorker.hpp"

#include "moviebackend/Errors.hpp"
#include "moviebackend/Logging.hpp"

namespace moviebackend {

PersistenceWorker::PersistenceWorker(Catalog& catalog,
                                     CatalogStore& store,
                                     std::chrono::milliseconds debounce)
    : m_catalog(catalog)
    , m_store(store)
    , m_debounce(debounce)
{
    // Register before starting the thread so that no change can slip through
    // the gap between "thread running" and "listener attached".
    m_catalog.setChangeListener(this);

    // Starting the thread is the last thing the constructor does: every
    // member it touches is fully constructed by now.
    //
    // Modern C++ note: the thread entry point is given as a pointer-to-member
    // plus `this` rather than a lambda. std::thread has supported this since
    // C++11 and it keeps the entry point a named, greppable function.
    m_thread = std::thread(&PersistenceWorker::run, this);

    MB_LOG_INFO("persistence worker started (debounce {} ms)", debounce.count());
}

PersistenceWorker::~PersistenceWorker()
{
    // 1. Stop receiving notifications. After this returns, no request thread
    //    can call onCatalogChanged() on a half-destroyed object.
    m_catalog.setChangeListener(nullptr);

    // 2. Ask the writer thread to finish.
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_stopRequested = true;
    }
    // Signalled outside the lock: the waiter cannot run until it reacquires
    // the mutex anyway, so notifying here avoids a pointless wake-and-block.
    m_condition.notify_all();

    // 3. Join. Never detach - a detached thread writing to m_store while the
    //    store is being destroyed is a use-after-free waiting to happen.
    if (m_thread.joinable()) {
        m_thread.join();
    }

    // 4. Final flush. The thread may have exited with a change still pending
    //    (the stop predicate wins over the debounce on purpose), so this is
    //    what guarantees a clean shutdown loses nothing.
    bool stillDirty = false;
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        stillDirty = m_dirty;
    }
    if (stillDirty) {
        MB_LOG_INFO("persistence worker: final flush on shutdown");
        performSave();
    }

    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        if (m_failedSaveCount > 0) {
            MB_LOG_WARN("persistence worker stopped after {} save(s) and {} "
                        "failed attempt(s)", m_saveCount, m_failedSaveCount);
        } else {
            MB_LOG_INFO("persistence worker stopped after {} save(s)", m_saveCount);
        }
    }
}

void PersistenceWorker::onCatalogChanged() noexcept
{
    {
        // CRITICAL SECTION: PersistenceWorker::m_mutex.
        //
        // The predicate state and the notification are both under the lock,
        // which is what closes the lost-wakeup race: the writer cannot test
        // m_dirty between this assignment and the notify below.
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_dirty = true;
        ++m_pendingGeneration;
    }
    m_condition.notify_all();
}

bool PersistenceWorker::waitForIdle(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    // Capture what "caught up" means at the moment of the call. Changes made
    // *after* this line are explicitly not waited for - otherwise a busy
    // service would make this function never return.
    const std::uint64_t target = m_pendingGeneration;

    // The predicate overload of wait_for. It re-tests after every wake-up, so
    // a spurious wake-up simply goes back to sleep, and it returns false only
    // on a genuine timeout.
    //
    // Written as a small lambda because std::condition_variable's predicate
    // parameter requires a callable - this is one of the two or three places
    // in the codebase where a library interface leaves no alternative.
    const bool caughtUp = m_condition.wait_for(
        lock, timeout,
        [this, target]() { return m_savedGeneration >= target || m_stopRequested; });

    return caughtUp;
}

std::uint64_t PersistenceWorker::saveCount() const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return m_saveCount;
}

void PersistenceWorker::run()
{
    for (;;) {
        std::uint64_t generationBeingSaved = 0;

        {
            std::unique_lock<std::mutex> lock(m_mutex);

            // ---- Step 1: sleep until there is something to do -------------
            //
            // Zero CPU cost while idle. The predicate covers both reasons to
            // wake: work arrived, or we are shutting down.
            m_condition.wait(lock,
                             [this]() { return m_dirty || m_stopRequested; });

            if (m_stopRequested) {
                // Leave whatever is pending to the destructor's final flush.
                //
                // The condition used to be `m_stopRequested && !m_dirty`, which
                // deadlocked shutdown once a failing save started re-arming
                // m_dirty: the flag was permanently true, so this exit was
                // never taken and the loop spun on retry-fail-retry forever
                // while the destructor waited in join().
                return;
            }

            // ---- Step 2: debounce -----------------------------------------
            //
            // Wait out a quiet period so that a burst of edits collapses into
            // one write. wait_for returns early if the predicate becomes true,
            // so a stop request is never delayed by the debounce.
            //
            // The predicate deliberately does NOT include m_dirty: it is
            // already true, and including it would make the wait return
            // instantly and defeat the debounce entirely.
            if (m_debounce.count() > 0) {
                m_condition.wait_for(lock, m_debounce,
                                     [this]() { return m_stopRequested; });
            }

            // ---- Step 3: take ownership of the pending work ---------------
            //
            // Clear the flag *before* saving, not after. If another change
            // lands while the file is being written, m_dirty goes true again
            // and we make one more pass - which is correct. Clearing it
            // afterwards would swallow that change.
            m_dirty = false;
            generationBeingSaved = m_pendingGeneration;
        }
        // ---- lock released -------------------------------------------------

        // Step 4: the actual work, with no lock held. The snapshot below
        // takes the *catalog's* lock, and file I/O follows; doing either of
        // those under m_mutex would block every request thread that wants to
        // report a change.
        const bool saved = performSave();

        {
            const std::lock_guard<std::mutex> guard(m_mutex);

            if (saved) {
                if (generationBeingSaved > m_savedGeneration) {
                    m_savedGeneration = generationBeingSaved;
                }
                ++m_saveCount;
            } else {
                // The write failed, so this generation did NOT reach the
                // store. Three things follow, and all three matter:
                //
                //  * m_savedGeneration must not advance, or waitForIdle()
                //    would report success for data that was never written;
                //  * m_dirty goes back up, so the change is retried rather
                //    than silently dropped (step 3 cleared it optimistically);
                //  * the destructor's final flush now has something to do.
                //
                // Before this, a full disk meant losing every booking made
                // since the last successful write while the shutdown log still
                // said everything was fine.
                m_dirty = true;
                ++m_failedSaveCount;
            }
        }
        // Wake anybody blocked in waitForIdle().
        m_condition.notify_all();

        if (!saved) {
            // Back off before the retry, so a permanently unwritable store
            // does not turn this thread into a busy loop. wait_for returns
            // early on a stop request, so shutdown is never delayed by it.
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_stopRequested) {
                return;
            }
            m_condition.wait_for(lock, kRetryBackoff,
                                 [this]() { return m_stopRequested; });
        }
    }
}

bool PersistenceWorker::performSave() noexcept
{
    // noexcept because this runs on a thread with nobody to catch anything:
    // an escaping exception would call std::terminate and take the whole
    // service down over a failed disk write. A failed save is logged and the
    // service keeps serving from memory.
    try {
        const CatalogSnapshot snapshot = m_catalog.snapshot();
        m_store.save(snapshot);
        return true;
    } catch (const ServiceError& error) {
        MB_LOG_ERROR("persistence worker: save failed ({}): {}",
                     toString(error.code()), error.what());
    } catch (const std::exception& error) {
        MB_LOG_ERROR("persistence worker: save failed: {}", error.what());
    } catch (...) {
        MB_LOG_ERROR("persistence worker: save failed with an unknown exception");
    }
    return false;
}

} // namespace moviebackend
