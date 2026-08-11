/**
 * @file IdGenerator.hpp
 * @brief Monotonic, prefixed identifier generator.
 */
#ifndef MOVIEBACKEND_IDGENERATOR_HPP
#define MOVIEBACKEND_IDGENERATOR_HPP

#include <cstdint>
#include <mutex>
#include <string>

namespace moviebackend {

/**
 * @brief Produces identifiers of the form `<prefix><n>`: "m1", "m2", ...
 *
 * ## Thread safety
 *
 * This type is safe to call from any thread. It owns a small mutex of its
 * own rather than relying on the Catalog lock, because the Catalog hands ids
 * out while already holding its lock *and* the seeding path runs before the
 * catalog exists. A `std::atomic` counter would also work; the mutex is used
 * for consistency with the rest of the codebase, and the contention here is
 * negligible.
 *
 * CRITICAL SECTION: IdGenerator::m_mutex, held only for the increment.
 */
class IdGenerator {
public:
    /**
     * @param prefix the letter(s) placed in front of the counter, e.g. "m"
     */
    explicit IdGenerator(std::string prefix);

    /** @return the next identifier, e.g. "m7". */
    std::string next();

    /**
     * @brief Raise the counter so that future ids do not collide with an id
     *        that already exists.
     *
     * Called while loading the catalog from disk: if the file contains "m42",
     * the generator must not later hand out "m42" again. Ids that do not
     * match this generator's prefix, or that have a non-numeric tail, are
     * ignored.
     *
     * @param existingId an id read from persistent storage
     */
    void observeExistingId(const std::string& existingId);

    /** @return the current counter value (last handed-out number). */
    std::uint64_t current() const;

private:
    mutable std::mutex m_mutex;
    std::string        m_prefix;
    std::uint64_t      m_counter = 0;
};

} // namespace moviebackend

#endif // MOVIEBACKEND_IDGENERATOR_HPP
