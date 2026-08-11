/**
 * @file main.cpp
 * @brief Process entry point: configuration, object lifetimes, shutdown.
 *
 * =========================================================================
 *  OBJECT LIFETIMES - the one thing to get right in this file
 * =========================================================================
 *
 * Several objects here hold *references* to each other rather than owning
 * them, which is only safe because construction order guarantees the
 * referent outlives the holder. Objects are destroyed in reverse order of
 * construction, so the order below is the whole safety argument:
 *
 *     1. Catalog            <- outlives everything; owns the state and the lock
 *     2. JsonCatalogStore   <- outlives the worker and the admin service
 *     3. PersistenceWorker  <- registers itself with the catalog, starts a thread
 *     4. BookingService     <- references the catalog
 *     5. AdminService       <- references the catalog and the store
 *     6. RestServer         <- references both services, starts accepting
 *
 * Destruction therefore runs 6 -> 1: the server stops accepting first, then
 * the services go, then the worker stops its thread and flushes, then the
 * store, then the catalog. At no point does a live object hold a reference to
 * a destroyed one.
 *
 * Everything is a stack object or a std::unique_ptr. There is no shared
 * ownership in this file.
 */
#include "moviebackend/AdminService.hpp"
#include "moviebackend/BookingService.hpp"
#include "moviebackend/Catalog.hpp"
#include "moviebackend/CatalogStore.hpp"
#include "moviebackend/Config.hpp"
#include "moviebackend/Errors.hpp"
#include "moviebackend/Logging.hpp"
#include "moviebackend/PersistenceWorker.hpp"
#include "moviebackend/RestServer.hpp"

#include "oatpp/core/base/Environment.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

/**
 * @brief Set when SIGINT/SIGTERM arrives, or when main() is done waiting.
 *
 * `std::atomic<bool>` is lock-free on every platform we target, and a
 * lock-free atomic store is one of the very few things a signal handler is
 * actually allowed to do.
 */
std::atomic<bool> g_shutdownRequested{false};

/**
 * @brief CALLBACK - SIGINT / SIGTERM handler.
 *
 * Async-signal-safety is the constraint that shapes this function, and it is
 * a harsher constraint than it looks. A handler runs by hijacking whichever
 * thread happened to receive the signal, so it may interrupt that thread at
 * *any* instruction - including one holding a mutex. Calling anything that
 * takes that same mutex then self-deadlocks, and the process hangs with no
 * stack trace to explain why.
 *
 * That rules out malloc, printf, and - the one that bit this project - the
 * logger: spdlog's thread-safe sinks take a mutex and allocate. Calling
 * RestServer::requestStop() directly from here was a latent deadlock, because
 * it logs.
 *
 * So the handler does exactly one legal thing: a lock-free atomic store. The
 * actual shutdown is performed by watchForShutdown() below, on an ordinary
 * thread where locking is fine.
 *
 * @param signalNumber the signal being delivered (unused)
 */
extern "C" void handleShutdownSignal(int signalNumber)
{
    (void)signalNumber;
    g_shutdownRequested.store(true);
}

/**
 * @brief Watches the shutdown flag and performs the actual stop.
 *
 * Runs on its own thread for the reason given above: everything
 * RestServer::requestStop() does - taking locks, logging, closing a socket -
 * is safe on a normal thread and unsafe in a signal handler.
 *
 * Polling rather than a condition variable is deliberate: notifying a CV is
 * itself not async-signal-safe, so the handler could not wake us. A 100 ms
 * poll costs nothing measurable and bounds Ctrl+C latency at 100 ms.
 *
 * The loop also exits when main() sets the flag itself after the accept loop
 * has returned for some other reason, so this thread is always joinable.
 *
 * @param server the server to stop; must outlive this thread
 */
void watchForShutdown(moviebackend::RestServer* server)
{
    while (!g_shutdownRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (server != nullptr) {
        server->requestStop();
    }
}

/**
 * @brief Read the config file path from the command line.
 *
 * Usage: `moviebackend [path/to/config.json]`
 *
 * @return the given path, or "config/config.json" when none was supplied
 */
std::string resolveConfigPath(int argc, char** argv)
{
    if (argc > 1) {
        return std::string(argv[1]);
    }
    return std::string("config/config.json");
}

} // unnamed namespace

int main(int argc, char** argv)
{
    using namespace moviebackend;

    /* =====================================================================
     * Configuration and logging - before anything else, so that every
     * subsequent step is logged at the level the operator asked for.
     * ===================================================================== */
    Config config;
    const std::string configPath = resolveConfigPath(argc, argv);

    try {
        config = Config::loadFromFile(configPath);
    } catch (const ServiceError& error) {
        // The logger does not exist yet, so this one message goes to stderr.
        std::cerr << "moviebackend: cannot load configuration: "
                  << error.what() << "\n"
                  << "usage: " << argv[0] << " [path/to/config.json]\n";
        return EXIT_FAILURE;
    }

    try {
        initialiseLogging(config.logging);
    } catch (const ServiceError& error) {
        std::cerr << "moviebackend: cannot initialise logging: "
                  << error.what() << "\n";
        return EXIT_FAILURE;
    }

    MB_LOG_INFO("moviebackend starting (config '{}')", configPath);
    MB_LOG_INFO("theater default capacity {} seats, {} per row",
                config.theaterDefaults.seatCapacity,
                config.theaterDefaults.seatsPerRow);

    // oatpp's process-wide state. Paired with destroy() at the very end.
    oatpp::base::Environment::init();

    int exitCode = EXIT_SUCCESS;

    try {
        /* =================================================================
         * Construction, in the order documented at the top of this file.
         * ================================================================= */

        // 1. The catalog - the state and the lock.
        Catalog catalog(config.theaterDefaults);

        // 2. The persistence backend.
        JsonCatalogStore store(config.catalog.path, config.theaterDefaults);

        // Load whatever is on disk. A missing file is not an error; it just
        // means an empty catalog, so a first run works out of the box.
        try {
            catalog.restore(store.load());
        } catch (const ServiceError& error) {
            // A *corrupt* file, on the other hand, is worth refusing to start
            // over - silently discarding somebody's catalog would be worse.
            MB_LOG_CRITICAL("cannot load catalog '{}': {}",
                            config.catalog.path, error.what());
            MB_LOG_CRITICAL("fix the file or point catalog.path elsewhere");
            oatpp::base::Environment::destroy();
            shutdownLogging();
            return EXIT_FAILURE;
        }

        // 3. The background writer. Only created when autosave is on; when it
        //    is off, nothing observes the catalog and saving is manual via
        //    POST /api/v1/admin/catalog/save.
        //
        //    unique_ptr because its existence is conditional - this is the
        //    only heap allocation in main().
        std::unique_ptr<PersistenceWorker> persistenceWorker;
        if (config.catalog.autosave) {
            persistenceWorker.reset(new PersistenceWorker(
                catalog, store,
                std::chrono::milliseconds(config.catalog.autosaveDebounceMs)));
        } else {
            MB_LOG_WARN("autosave is disabled - the catalog is only written "
                        "when POST /api/v1/admin/catalog/save is called");
        }

        // 4 & 5. The two service facades.
        BookingService bookingService(catalog);
        AdminService adminService(catalog, store, config.schedule);

        // 6. The REST/IPC boundary. Binding happens in the constructor, so a
        //    port clash is reported before we install the signal handlers.
        RestServer server(config.server, bookingService, adminService);

        /* =================================================================
         * Signal handling - installed only once the server exists.
         * ================================================================= */
        std::thread signalWatcher(watchForShutdown, &server);

        std::signal(SIGINT, handleShutdownSignal);
        std::signal(SIGTERM, handleShutdownSignal);

        MB_LOG_INFO("listening on http://{}:{} - press Ctrl+C to stop",
                    config.server.host, server.boundPort());
        MB_LOG_INFO("end-user API under /api/v1, admin API under /api/v1/admin");

        // Blocks here for the lifetime of the service.
        server.start();

        /* =================================================================
         * Shutdown.
         * ================================================================= */
        MB_LOG_INFO("shutting down");

        // Restore the default handlers before the watcher goes away, so a
        // second Ctrl+C during shutdown terminates rather than being ignored.
        std::signal(SIGINT, SIG_DFL);
        std::signal(SIGTERM, SIG_DFL);

        // Setting the flag releases the watcher even when start() returned for
        // a reason other than a signal, so the join below always completes.
        g_shutdownRequested.store(true);
        signalWatcher.join();

        // Flush any pending change synchronously, so that a clean shutdown
        // never loses a booking. The worker's destructor would do this too,
        // but doing it here means a failure is logged while the logger is
        // still configured.
        if (persistenceWorker != nullptr) {
            if (!persistenceWorker->waitForIdle(std::chrono::seconds(5))) {
                MB_LOG_WARN("persistence worker did not finish within 5s");
            }
        }

        // Everything above is destroyed here, in reverse order.

    } catch (const ServiceError& error) {
        MB_LOG_CRITICAL("fatal: {} ({})", error.what(), toString(error.code()));
        exitCode = EXIT_FAILURE;
    } catch (const std::exception& error) {
        MB_LOG_CRITICAL("fatal: {}", error.what());
        exitCode = EXIT_FAILURE;
    } catch (...) {
        MB_LOG_CRITICAL("fatal: unknown exception");
        exitCode = EXIT_FAILURE;
    }

    // Release oatpp's process-wide state after the last oatpp object is gone.
    // Doing this explicitly is what keeps the Valgrind memcheck output clean.
    oatpp::base::Environment::destroy();

    MB_LOG_INFO("moviebackend stopped");
    shutdownLogging();

    return exitCode;
}
