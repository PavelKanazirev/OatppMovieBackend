# OatppMovieBackend
Movie backend microservice using Oat++ FOSS

---

## Table of contents

1. [What it does]
2. [Prerequisites and setup, per platform]
3. [Build]
4. [Run]
5. [Test]
6. [QA: Valgrind, docs, diagrams]
7. [Configuration]
8. [REST API reference]
9. [Repository layout]
10. [Acknowledgements]

---

## What it does

**End users** can browse and book:

```
view all playing movies -> select a movie -> see theaters showing it
  -> select a theater -> see its showtimes -> see available seats -> book seats
```

**A special (administrative) user** can edit the catalog at run time through a
separate set of routes under `/api/v1/admin`: append, modify and remove
theaters, movies and individual time slots, and generate a default timeline for
a movie in a theater.

Everything lives in memory for the lifetime of the process. There is no DBMS.
The state is checkpointed to a human-editable JSON file by a background thread.

Seat capacity defaults to **20** per theater and is configurable, both globally
in `config/config.json` and per theater.

---

## Prerequisites and setup, per platform

### WSL / Ubuntu on Windows 11

Tested on Ubuntu 26.04 with GCC 15.2.

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    libboost-json-dev \
    libgtest-dev \
    python3 \
    valgrind \
    doxygen \
    plantuml
#optional
sudo apt install -y 'ninja-build'
```

| Package | Why it is needed | If you skip it |
|---|---|---|
| `build-essential` | GCC 13+ is required for C++23 | build fails |
| `cmake` | 3.22 or newer | configure fails |
| `git` | oat++ and spdlog are fetched from GitHub | configure fails |
| `libboost-json-dev` | JSON serialisation | fetched from GitHub instead |
| `libgtest-dev` | unit tests | fetched from GitHub instead |
| `python3` | smoke tests | smoke tests are skipped |
| `valgrind` | memcheck targets | memcheck targets are not created |
| `doxygen` | API documentation | `docs` target is not created |
| `plantuml` | rendering the diagrams | `diagrams` target is not created; the `.puml` sources stay readable |
| `ninja-build` | 1.10 or newer | optimised/quicker build fails

Only the first three are genuinely required — everything else is either
downloaded automatically or degrades to an optional target that is simply not
defined.

**Network access is required on the first configure**, because oat++ has no
distribution package and is always fetched. Subsequent builds are offline.

Check your toolchain is new enough:

```bash
g++ --version     # need 13 or newer for C++23
cmake --version   # need 3.22 or newer
```

### Windows 11 (native, MSVC)

Install:

1. **Visual Studio 2022**, version 17.6 or newer, with the *Desktop development
   with C++* workload. Older versions lack sufficient C++23 support. The
   workload includes CMake and Ninja.
2. **Git for Windows** — <https://git-scm.com/download/win>
3. **Python 3.8+** — <https://www.python.org/downloads/> — only for the smoke
   tests. Tick *Add python.exe to PATH* during installation.
4. *(optional)* **Doxygen** — <https://www.doxygen.nl/download.html>
5. *(optional)* **PlantUML** — needs a JRE; set the `PLANTUML_JAR` environment
   variable to the path of `plantuml.jar`.
6. *(optional)* **Ninja** — <https://github.com/ninja-build/ninja/releases> —
   a fast build tool. Install it if `ninja --version` is not available from your terminal.

Boost and GoogleTest do **not** need installing on Windows: neither is found by
`find_package`, so both are fetched and built automatically.

Open **"x64 Native Tools Command Prompt for VS 2022"** — a plain `cmd.exe` will
not have the compiler on its `PATH` — and work from there.

> Valgrind does not exist on Windows. The memcheck targets are simply not
> defined there; see [QA](#qa-valgrind-docs-diagrams) for what to use instead.

---

## Build

### WSL / Ubuntu

```bash
git clone <repository-url> OatppMovieBackend
cd OatppMovieBackend

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

The first configure downloads oat++ (and anything else not found on the
system), so it takes a few minutes. Later configures are offline and fast.

For a release build — which also strips every log call below *error* out of the
binary at compile time:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)
```

### Windows 11

From the *x64 Native Tools Command Prompt*:

```bat
git clone <repository-url> MovieBackend
cd MovieBackend

cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug
```

Or with Ninja, which is considerably faster:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The executable lands in `build\bin\Debug\moviebackend.exe` (Visual Studio
generator) or `build\bin\moviebackend.exe` (Ninja).

### Build options

| Option | Default | Meaning |
|---|---|---|
| `MOVIEBACKEND_BUILD_TESTS` | `ON` | build the GoogleTest suite |
| `MOVIEBACKEND_BUILD_DOCS` | `ON` | define the `docs` target if Doxygen is present |
| `MOVIEBACKEND_WARNINGS_AS_ERRORS` | `OFF` | turn warnings into errors |

---

## Run
# WSL
```bash
cd build/bin          # the config/ directory is copied here at build time
./moviebackend
```

```bat
REM Windows
cd build\bin\Debug
moviebackend.exe
```

The server listens on `0.0.0.0:8000` by default and prints its address:

```
[info] listening on http://0.0.0.0:8000 - press Ctrl+C to stop
[info] end-user API under /api/v1, admin API under /api/v1/admin
```

A different configuration file can be passed as the only argument:
# WSL
```bash
./moviebackend /path/to/my-config.json
```

Stop it with **Ctrl+C**. The shutdown is graceful: the accept loop ends,
in-flight requests finish, and any pending catalog change is flushed to disk
before the process exits.

Quick check that it is alive:
# WSL
```bash
curl http://localhost:8000/api/v1/health
curl http://localhost:8000/api/v1/movies
```

---

## Test

### Unit tests (GoogleTest)

```bash
cd build
ctest --output-on-failure          # runs the unit tests AND the Python smoke tests
```

Or run the binary directly, which gives finer control:

```bash
./build/bin/moviebackend_tests
./build/bin/moviebackend_tests --gtest_filter='CatalogConcurrencyTest.*'
```

The tests are quiet by default. To see the service's own logging while a test
runs:

```bash
MOVIEBACKEND_TEST_LOG_LEVEL=debug ./build/bin/moviebackend_tests
```

The suite that matters most is
[`tests/unit/CatalogConcurrencyTest.cpp`](tests/unit/CatalogConcurrencyTest.cpp).
It uses a condition-variable *starting gate* so that every worker thread hits
the catalog within microseconds of the others, then asserts the invariant a
race would break:

> successful bookings == seats actually taken == seats no longer available

### Smoke tests (Python)

These start the **real** server binary and talk to it over **real HTTP**,
covering everything the unit tests deliberately skip: request parsing, routing,
JSON mapping, and the exception-to-status-code translation.

# WSL
```bash
python3 tests/smoke/run_smoke_tests.py --binary build/bin/moviebackend --source-dir .
```

```bat
REM Windows
python tests\smoke\run_smoke_tests.py --binary build\bin\Debug\moviebackend.exe --source-dir .
```

They need **no third-party Python packages** — standard library only, so a bare
Python 3.8 works. The server is started on an ephemeral port (`"port": 0`) with
a throwaway copy of the catalog, so running them never disturbs a server you
already have running, nor the checked-in `config/catalog.json`.

Included is an end-to-end version of the over-booking test: 16 threads,
released simultaneously by a `threading.Barrier`, all requesting the same seat.
Exactly one must receive `201`; the other fifteen must receive
`409 SEAT_UNAVAILABLE`.

---

## QA: Valgrind, docs, diagrams

### Valgrind (Linux / WSL only)

Valgrind does not exist on Windows, so these targets are only defined on Linux.
In CI they belong on the Linux agent; the Windows agent covers the same ground
with `/fsanitize=address` or Application Verifier. Because the code is plain
portable C++ with no platform-specific memory handling, a clean memcheck run on
Linux is strong evidence for the Windows build too.

Two targets, answering two different questions:

```bash
cmake --build build --target memcheck-tests    # does the logic leak?
cmake --build build --target memcheck-server   # does a real request cycle leak?
```

* **`memcheck-tests`** runs the GoogleTest binary under memcheck. Deterministic
  and safe to gate CI on.
* **`memcheck-server`** starts the real server under memcheck, drives it with
  the Python smoke tests, then shuts it down with `SIGINT` so destructors
  actually run — which is the whole point. A `SIGKILL` would leave the entire
  live heap looking like a leak.

### API documentation (Doxygen)

# WSL
```bash
cmake --build build --target docs
```

Then open `build/docs/html/index.html`. The build is warning-clean, and
`WARN_IF_UNDOCUMENTED` is on so it stays that way.

### Diagrams (PlantUML)

```bash
cmake --build build --target diagrams
```

Renders to `build/docs/diagrams/` as both SVG and PNG. The sources are in
[`docs/plantuml/`](docs/plantuml/) and are readable as plain text without
rendering.

| Diagram | What it shows |
|---|---|
| `01-booking-flow` | the booking request end to end, with the critical section marked |
| `02-browse-flow` | view movies -> select movie -> select theater -> view seats |
| `03-ipc-protocol` | the IPC handshake: bind, accept, route, respond, shut down |
| `04-concurrent-booking` | **two clients racing for one seat, and why only one wins** |
| `05-persistence-worker` | the mutex + condition_variable producer/consumer |
| `06-admin-schedule` | admin edits and default-timeline generation |
| `07-component-architecture` | the layers and the dependency rule |
| `08-domain-model` | the value types and what actually gets booked |
| `09-process-lifecycle` | construction and destruction order |
| `10-seat-state` | the state machine of one seat |

---

## Configuration

`config/config.json` — every field is optional, and an empty `{}` yields the
documented defaults:

```json
{
  "server":  { "host": "0.0.0.0", "port": 8000 },
  "theater_defaults": { "seat_capacity": 20, "seats_per_row": 10 },
  "schedule": { "day_start": "09:00", "day_end": "21:00" },
  "catalog": {
    "path": "config/catalog.json",
    "autosave": true,
    "autosave_debounce_ms": 500
  },
  "logging": {
    "level": "debug",
    "file": "",
    "pattern": "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v"
  }
}
```

| Field | Default | Notes |
|---|---|---|
| `server.port` | `8000` | `0` asks the OS for a free port |
| `theater_defaults.seat_capacity` | `20` | **the required default**; a theater may override it |
| `theater_defaults.seats_per_row` | `10` | decides the labelling: 20 seats at 10/row gives `a1`..`a10`, `b1`..`b10` |
| `schedule.day_start` / `day_end` | `09:00` / `21:00` | bounds for generated and manually added slots |
| `catalog.path` | `config/catalog.json` | the file the admin API edits |
| `catalog.autosave` | `true` | when `false`, saving is only via `POST /admin/catalog/save` |
| `catalog.autosave_debounce_ms` | `500` | a burst of edits produces one write, not one per edit |
| `logging.level` | `info` | `trace`/`debug`/`info`/`warn`/`error`/`critical`/`off` |

A malformed file names the offending field:

```
config.json: field 'server.port' must be 0..65535
```

**Seat labelling.** Seats are laid out row by row: `a1`..`a10`, then
`b1`..`b10`. A partially filled last row is fine — capacity 12 at 10 per row
gives `a1`..`a10`, `b1`, `b2`, and `b3` does not exist. Rows are limited to
`a`..`z`, so a layout needing more than 26 rows is rejected at load time.

---

## REST API reference

### End-user API

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/v1/health` | liveness check |
| `GET` | `/api/v1/movies` | all **playing** movies (those with at least one showtime) |
| `GET` | `/api/v1/movies/{movieId}` | one movie |
| `GET` | `/api/v1/movies/{movieId}/theaters` | theaters showing it |
| `GET` | `/api/v1/movies/{movieId}/theaters/{theaterId}/showtimes` | its timeline there, with seat counts |
| `GET` | `/api/v1/showtimes/{showtimeId}/seats` | available **and** booked seats |
| `POST` | `/api/v1/showtimes/{showtimeId}/bookings` | **book seats** |
| `GET` | `/api/v1/bookings/{bookingId}` | the receipt |
| `DELETE` | `/api/v1/bookings/{bookingId}` | cancel, releasing the seats |

### Administrative API

| Method | Path | Purpose |
|---|---|---|
| `GET` `POST` | `/api/v1/admin/theaters` | list / append a theater |
| `PUT` `DELETE` | `/api/v1/admin/theaters/{theaterId}` | modify / remove |
| `GET` `POST` | `/api/v1/admin/movies` | list (**including unscheduled**) / append |
| `PUT` `DELETE` | `/api/v1/admin/movies/{movieId}` | modify / remove |
| `GET` | `/api/v1/admin/showtimes` | every showtime |
| `GET` `POST` | `/api/v1/admin/movies/{m}/theaters/{t}/showtimes` | list / append one time slot |
| `POST` | `/api/v1/admin/movies/{m}/theaters/{t}/default-schedule` | **generate the default timeline** |
| `PUT` `DELETE` | `/api/v1/admin/showtimes/{showtimeId}` | move / remove a time slot |
| `POST` | `/api/v1/admin/catalog/save` | force a synchronous checkpoint |
| `POST` | `/api/v1/admin/catalog/reload` | re-read the file from disk |

### Error responses

Every failure — ours or the framework's — uses the same envelope:

```json
{ "code": "SEAT_UNAVAILABLE", "message": "seat 'a1' is already booked" }
```

| `code` | HTTP | Meaning |
|---|---|---|
| `NOT_FOUND` | 404 | no such movie / theater / showtime / seat / booking |
| `INVALID_ARGUMENT` | 400 | malformed request |
| `CONFLICT` | 409 | clashes with current state (duplicate id, occupied slot) |
| `SEAT_UNAVAILABLE` | 409 | somebody else got the seat first |
| `PERSISTENCE_FAILURE` | 500 | could not read or write the catalog file |
| `INTERNAL` | 500 | unanticipated |

`SEAT_UNAVAILABLE` shares 409 with `CONFLICT` but keeps its own code, so a
client can tell "retry with different seats" from "your request was
inconsistent".

### The default timeline rule

For a movie of duration *D* in a theater, with the day running
`day_start`..`day_end`:

* the first screening starts at `day_start` (09:00);
* each following one starts at the first **whole hour at or after** the
  previous screening *ends*;
* slots are emitted while the start time is <= `day_end` (21:00) — a film may
  run past it, only the start time is capped.

A 110-minute film therefore gets `09:00, 11:00, 13:00, 15:00, 17:00, 19:00,
21:00`.

A film ending *exactly* on the hour gets a back-to-back slot: a 120-minute film
starting at 09:00 ends at 11:00, and the next slot is **11:00, not 12:00**.
"A round hour after the timeline" is read as "at or after".
---

## Repository layout

```
include/moviebackend/   public API headers - the documented consumer interface
src/core/               Catalog (the lock), SeatMap, Schedule, Config, Errors
src/service/            BookingService, AdminService - the use cases
src/persistence/        Boost.JSON store + the background writer thread
src/rest/               oat++ controllers, DTOs, error handler, server wiring
src/main.cpp            configuration, object lifetimes, signal handling
tests/unit/             GoogleTest suite (135 tests)
tests/smoke/            Python end-to-end tests (70 assertions)
docs/plantuml/          10 PlantUML diagrams
cmake/                  dependency resolution, warnings, Valgrind, Doxygen, PlantUML
config/                 config.json and the seed catalog.json
scripts/                the memcheck driver
```

---

## Acknowledgements

This project uses [Oat++](https://oatpp.io/), an open-source C++ web framework,
to provide its REST API layer.

Oat++ is developed and maintained by the
[Oat++ contributors](https://github.com/oatpp/oatpp/graphs/contributors)
and is licensed under the
[Apache License 2.0](https://github.com/oatpp/oatpp/blob/master/LICENSE).

The implementation and project setup were also informed by
[C++ Oatpp Web Framework — Ashton Bradley](https://youtu.be/UzSZaeyoN-w).