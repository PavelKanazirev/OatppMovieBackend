/**
 * @file TestSupport.hpp
 * @brief Helpers shared by the unit tests.
 */
#ifndef MOVIEBACKEND_TESTSUPPORT_HPP
#define MOVIEBACKEND_TESTSUPPORT_HPP

#include "moviebackend/CatalogStore.hpp"
#include "moviebackend/Domain.hpp"
#include "moviebackend/Errors.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief Assert that a statement throws a ServiceError carrying @p expected.
 *
 * gtest's EXPECT_THROW cannot inspect the exception, and the error *code* is
 * the part of the contract that matters - a NotFound where a Conflict was
 * meant is a real API bug that EXPECT_THROW would happily pass.
 */
#define EXPECT_SERVICE_ERROR(statement, expected)                             \
    do {                                                                      \
        try {                                                                 \
            statement;                                                        \
            FAIL() << "expected a ServiceError, none was thrown";             \
        } catch (const ::moviebackend::ServiceError& error) {                 \
            EXPECT_EQ(expected, error.code()) << error.what();                \
        }                                                                     \
    } while (false)

namespace moviebackend {
namespace testsupport {

/**
 * @brief A CatalogStore that keeps everything in memory.
 *
 * Design pattern: **Test Double** (a fake, not a mock - it has working
 * behaviour). Lets the persistence tests run with no filesystem at all, which
 * keeps them fast and free of temp-directory cleanup.
 *
 * Thread safe, because PersistenceWorker calls save() from its own thread
 * while the test thread reads the counters.
 */
class InMemoryCatalogStore : public CatalogStore {
public:
    InMemoryCatalogStore() = default;

    CatalogSnapshot load() override
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        ++m_loadCount;
        return m_snapshot;
    }

    void save(const CatalogSnapshot& snapshot) override
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        if (m_failNextSave || m_failAllSaves) {
            m_failNextSave = false;
            throw ServiceError(ErrorCode::PersistenceFailure,
                               "InMemoryCatalogStore was told to fail");
        }
        m_snapshot = snapshot;
        ++m_saveCount;
    }

    /** @brief Seed the store with state that load() will return. */
    void seed(const CatalogSnapshot& snapshot)
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_snapshot = snapshot;
    }

    /** @return the most recently saved (or seeded) state. */
    CatalogSnapshot stored() const
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        return m_snapshot;
    }

    std::size_t saveCount() const
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        return m_saveCount;
    }

    std::size_t loadCount() const
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        return m_loadCount;
    }

    /** @brief Make the next save() throw, to cover the error path. */
    void failNextSave()
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_failNextSave = true;
    }

    /**
     * @brief Make every save() throw until switched off again.
     *
     * Models a full or read-only disk, which is the case where "a failed write
     * is silently treated as success" turns into real data loss.
     */
    void failAllSaves(bool fail)
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_failAllSaves = fail;
    }

private:
    mutable std::mutex m_mutex;
    CatalogSnapshot    m_snapshot;
    std::size_t        m_saveCount = 0;
    std::size_t        m_loadCount = 0;
    bool               m_failNextSave = false;
    bool               m_failAllSaves = false;
};

/** @brief Build a Movie without repeating the field names in every test. */
inline Movie makeMovie(const std::string& id,
                       const std::string& title,
                       int durationMinutes)
{
    Movie movie;
    movie.id = id;
    movie.title = title;
    movie.durationMinutes = durationMinutes;
    movie.language = "EN";
    movie.genre = "Test";
    return movie;
}

/** @brief Build a Theater without repeating the field names in every test. */
inline Theater makeTheater(const std::string& id,
                           const std::string& name,
                           int seatCapacity,
                           int seatsPerRow)
{
    Theater theater;
    theater.id = id;
    theater.name = name;
    theater.city = "Testville";
    theater.seatCapacity = seatCapacity;
    theater.seatsPerRow = seatsPerRow;
    return theater;
}

/** @return true if @p values contains @p wanted. */
inline bool contains(const std::vector<std::string>& values,
                     const std::string& wanted)
{
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] == wanted) {
            return true;
        }
    }
    return false;
}

} // namespace testsupport
} // namespace moviebackend

#endif // MOVIEBACKEND_TESTSUPPORT_HPP
