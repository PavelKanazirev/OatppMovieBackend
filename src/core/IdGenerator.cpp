/**
 * @file IdGenerator.cpp
 * @brief Implementation of the prefixed id generator.
 */
#include "moviebackend/IdGenerator.hpp"

#include <utility>

namespace moviebackend {

IdGenerator::IdGenerator(std::string prefix)
    : m_prefix(std::move(prefix))
{
}

std::string IdGenerator::next()
{
    std::uint64_t value = 0;

    {
        // CRITICAL SECTION: the increment must be atomic with respect to
        // other threads, otherwise two requests could receive the same id.
        // Scoped deliberately tightly - the string building below does not
        // need the lock.
        const std::lock_guard<std::mutex> guard(m_mutex);
        ++m_counter;
        value = m_counter;
    }

    return m_prefix + std::to_string(value);
}

void IdGenerator::observeExistingId(const std::string& existingId)
{
    // Only ids of the shape "<prefix><digits>" are interesting. Anything else
    // (a hand-written id like "imax-hall") simply cannot collide with what we
    // generate, so it is ignored.
    if (existingId.size() <= m_prefix.size()) {
        return;
    }
    if (existingId.compare(0, m_prefix.size(), m_prefix) != 0) {
        return;
    }

    std::uint64_t parsed = 0;
    for (std::size_t i = m_prefix.size(); i < existingId.size(); ++i) {
        const char c = existingId[i];
        if (c < '0' || c > '9') {
            return; // non-numeric tail: not one of ours
        }

        const std::uint64_t digit =
            static_cast<std::uint64_t>(static_cast<unsigned char>(c) - '0');

        // Guard against a pathological id like "m99999999999999999999"
        // overflowing the counter. Anything that large is treated as
        // "not one of ours" rather than wrapping around.
        if (parsed > (UINT64_MAX - digit) / 10U) {
            return;
        }
        parsed = parsed * 10U + digit;
    }

    // CRITICAL SECTION: raise the counter so the next id cannot collide.
    const std::lock_guard<std::mutex> guard(m_mutex);
    if (parsed > m_counter) {
        m_counter = parsed;
    }
}

std::uint64_t IdGenerator::current() const
{
    const std::lock_guard<std::mutex> guard(m_mutex);
    return m_counter;
}

} // namespace moviebackend
