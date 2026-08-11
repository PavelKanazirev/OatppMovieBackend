/**
 * @file CatalogStore.hpp
 * @brief Loading and saving the catalog as JSON.
 *
 * ## Why Boost here, and only here
 *
 * The project restricts Boost to serialisation/deserialisation, and this is
 * that one place: **Boost.JSON** parses and writes the on-disk catalog and
 * the config file.
 *
 * Boost.JSON rather than Boost.Serialization because the artefact has to be a
 * *human-editable JSON file* - the exercise requires an administrator to edit
 * the catalog - and Boost.Serialization's archives are a C++-specific format,
 * not JSON. Boost.JSON also keeps real value types on a round trip, which
 * Boost.PropertyTree's JSON parser famously does not (it degrades every
 * value to a string, so a saved capacity of 20 would come back as "20").
 *
 * Note that oatpp's own JSON mapper handles the *wire* format of the REST
 * API. The two are intentionally separate: the wire DTOs may change shape for
 * API-versioning reasons without disturbing the storage format.
 */
#ifndef MOVIEBACKEND_CATALOGSTORE_HPP
#define MOVIEBACKEND_CATALOGSTORE_HPP

#include "moviebackend/Config.hpp"
#include "moviebackend/Domain.hpp"

#include <string>

namespace moviebackend {

/**
 * @brief Abstract persistence boundary for the catalog.
 *
 * Design pattern: **Strategy**. Catalog and PersistenceWorker depend on this
 * interface, never on the JSON implementation, which is what lets the unit
 * tests substitute an in-memory fake and assert on save behaviour without
 * touching a disk.
 */
class CatalogStore {
public:
    virtual ~CatalogStore() = default;

    /**
     * @brief Read the stored catalog.
     * @return the loaded state
     * @throws ServiceError PersistenceFailure if the source cannot be read,
     *         InvalidArgument if its content is not valid.
     */
    virtual CatalogSnapshot load() = 0;

    /**
     * @brief Write the catalog out.
     * @param snapshot state to persist
     * @throws ServiceError PersistenceFailure on any I/O problem.
     */
    virtual void save(const CatalogSnapshot& snapshot) = 0;

protected:
    CatalogStore() = default;
    CatalogStore(const CatalogStore&) = delete;
    CatalogStore& operator=(const CatalogStore&) = delete;
};

/**
 * @brief CatalogStore backed by a JSON file on disk.
 *
 * ## Crash safety
 *
 * save() writes to `<path>.tmp` and then renames it over `<path>`. A rename
 * is atomic on both POSIX and NTFS, so a crash mid-write leaves the previous
 * catalog intact rather than a half-written file. This matters more than
 * usual here because a background thread rewrites the file while the service
 * keeps serving requests.
 *
 * ## Thread safety
 *
 * Not thread safe by itself. It is only ever driven by PersistenceWorker's
 * single writer thread, plus the startup load before any thread exists.
 */
class JsonCatalogStore : public CatalogStore {
public:
    /**
     * @param path     the JSON file to read and write
     * @param defaults seating defaults, applied to entries in the file that
     *                 omit a capacity
     */
    JsonCatalogStore(std::string path, const TheaterDefaults& defaults);

    /**
     * @copydoc CatalogStore::load
     *
     * A missing file is **not** an error: it yields an empty catalog, so a
     * first run with no data file simply starts empty.
     */
    CatalogSnapshot load() override;

    /** @copydoc CatalogStore::save */
    void save(const CatalogSnapshot& snapshot) override;

    /** @return the file path this store reads and writes. */
    const std::string& path() const noexcept { return m_path; }

    /**
     * @brief Parse a catalog document held in memory.
     *
     * Exposed for the unit tests, which drive the parser directly.
     *
     * @param json            document text
     * @param defaults        seating defaults for entries that omit them
     * @param originForErrors label used in error messages
     * @throws ServiceError InvalidArgument if the document is malformed.
     */
    static CatalogSnapshot parse(const std::string& json,
                                 const TheaterDefaults& defaults,
                                 const std::string& originForErrors);

    /**
     * @brief Serialise a catalog to a pretty-printed JSON document.
     * @param snapshot state to serialise
     * @return the JSON text, ending in a newline
     */
    static std::string serialise(const CatalogSnapshot& snapshot);

private:
    std::string     m_path;
    TheaterDefaults m_defaults;
};

} // namespace moviebackend

#endif // MOVIEBACKEND_CATALOGSTORE_HPP
