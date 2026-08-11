MovieBackend API Overview                        {#mainpage}
=========================

MovieBackend is an in-memory service for reserving cinema seats. It keeps its
whole state in memory for the lifetime of the process, checkpoints that state
to a JSON file, and exposes two REST APIs over HTTP.

This page is the entry point of the generated documentation. The source of
truth for behaviour is the header comments, which are considerably more
detailed than this summary.

Layers
------

The code is deliberately layered, with each layer depending only on the one
below it:

| Layer       | Header(s)                              | Responsibility |
|-------------|----------------------------------------|----------------|
| REST / IPC  | `RestServer.hpp`                       | HTTP + JSON, the process boundary |
| Service     | `BookingService.hpp`, `AdminService.hpp` | The use cases, one class per audience |
| Core        | `Catalog.hpp`, `SeatMap.hpp`, `Schedule.hpp` | State, invariants, **the lock** |
| Persistence | `CatalogStore.hpp`, `PersistenceWorker.hpp` | JSON on disk, background writing |
| Support     | `Config.hpp`, `Logging.hpp`, `Errors.hpp`, `IdGenerator.hpp` | Cross-cutting |

Where to start reading
----------------------

* **moviebackend::Catalog** — read the comment block at the top of
  `Catalog.hpp` first. It explains the locking strategy that makes concurrent
  booking correct, which is the central problem this service solves.

* **moviebackend::Catalog::bookSeats** — the check-then-act sequence that
  makes over-booking impossible.

* **moviebackend::PersistenceWorker** — the one place a
  `std::condition_variable` is genuinely needed, written out as a textbook
  producer/consumer.

* **moviebackend::RestServer** — the IPC boundary, and the list of the three
  kinds of boundary crossing.

The two audiences
-----------------

End users go through **moviebackend::BookingService**:

    list playing movies -> pick a movie -> see theaters -> pick a theater
      -> see showtimes -> see free seats -> book

Administrators go through **moviebackend::AdminService**, which can append,
modify and remove theaters, movies and time slots, and can generate a
default timeline for a movie in a theater.

Error handling
--------------

Every layer signals failure by throwing moviebackend::ServiceError, which
carries a moviebackend::ErrorCode. The REST layer has exactly one place that
turns those codes into HTTP status codes.
