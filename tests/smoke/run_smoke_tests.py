#!/usr/bin/env python3
"""End-to-end smoke tests for the MovieBackend REST/IPC boundary.

Why these exist alongside the C++ unit tests
--------------------------------------------
The GoogleTest suite links against the service classes directly, so it never
touches a socket. Everything between "bytes arrive on a TCP connection" and
"BookingService is called" is therefore untested by it: HTTP parsing, routing,
path-parameter extraction, JSON deserialisation into DTOs, DTO serialisation
back out, and the exception-to-status-code mapping.

These tests drive the *real* server binary over *real* HTTP, in a separate
process, which is the only way to cover that path. They are the "Python-based
smoke tests exercising the backend/client IPC path end to end" deliverable.

Design notes
------------
* **Zero third-party dependencies.** Only the standard library is used
  (urllib, json, subprocess, threading), so the tests run on a bare Python 3.8+
  on Windows or Linux with nothing to install. `requests` would be nicer to
  read but would be one more thing to get wrong on a fresh machine.

* **An ephemeral port.** The server is started with `"port": 0`, so the OS
  picks a free one and the tests never collide with a server the developer
  already has running. The chosen port is read back from the server's log.

* **A private catalog.** Each run copies the catalog into a temporary
  directory, so the tests never modify the checked-in config/catalog.json.

Usage
-----
    python3 tests/smoke/run_smoke_tests.py --binary build/bin/moviebackend \\
                                           --source-dir .

Exit code 0 means every check passed.
"""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

# --------------------------------------------------------------------------
# Tiny test harness
#
# A hand-rolled harness rather than unittest, for one reason: the whole file
# must be readable top to bottom by somebody who does not know Python's test
# frameworks. It is about thirty lines.
# --------------------------------------------------------------------------

class Results:
    """Counts passes and failures and prints them as they happen."""

    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.failures = []

    def check(self, condition, description):
        """Record one assertion."""
        if condition:
            self.passed += 1
            print("  PASS  {}".format(description))
        else:
            self.failed += 1
            self.failures.append(description)
            print("  FAIL  {}".format(description))
        return condition

    def check_equal(self, actual, expected, description):
        ok = actual == expected
        if not ok:
            description = "{} (expected {!r}, got {!r})".format(
                description, expected, actual)
        return self.check(ok, description)


RESULTS = Results()


def section(title):
    print("\n=== {} ===".format(title))


# --------------------------------------------------------------------------
# HTTP helpers
# --------------------------------------------------------------------------

class Response:
    """A parsed HTTP response: status code plus decoded JSON body."""

    def __init__(self, status, body):
        self.status = status
        self.body = body


def request(base_url, method, path, payload=None, timeout=10):
    """Perform one HTTP request and return a Response.

    An HTTP error status is a normal result here, not an exception - the tests
    assert on 404 and 409 as much as on 200.
    """
    url = base_url + path
    data = None
    headers = {"Accept": "application/json"}

    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data, headers=headers, method=method)

    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            raw = response.read().decode("utf-8")
            return Response(response.status, _decode(raw))
    except urllib.error.HTTPError as error:
        raw = error.read().decode("utf-8")
        return Response(error.code, _decode(raw))


def _decode(raw):
    if not raw:
        return None
    try:
        return json.loads(raw)
    except ValueError:
        return raw


# --------------------------------------------------------------------------
# Server lifecycle
# --------------------------------------------------------------------------

class ServerProcess:
    """Starts the real backend on an ephemeral port and shuts it down cleanly."""

    # The server logs "listening on http://0.0.0.0:PORT" once it is bound.
    PORT_PATTERN = re.compile(r"listening on http://[^:]+:(\d+)")

    def __init__(self, binary, source_dir, workdir):
        self.binary = os.path.abspath(binary)
        self.source_dir = os.path.abspath(source_dir)
        self.workdir = workdir
        self.process = None
        self.port = None
        self.log_lines = []
        self._log_thread = None

    def _write_config(self):
        """Write a config that uses port 0 and a throwaway catalog copy."""
        catalog_source = os.path.join(self.source_dir, "config", "catalog.json")
        catalog_target = os.path.join(self.workdir, "catalog.json")
        shutil.copyfile(catalog_source, catalog_target)

        config = {
            # Loopback only and port 0: the OS picks a free port, so a
            # developer's already-running server is never disturbed.
            "server": {"host": "127.0.0.1", "port": 0},
            "theater_defaults": {"seat_capacity": 20, "seats_per_row": 10},
            "schedule": {"day_start": "09:00", "day_end": "21:00"},
            "catalog": {
                "path": catalog_target,
                "autosave": True,
                # Short debounce so the persistence assertions do not have to
                # wait half a second each.
                "autosave_debounce_ms": 50,
            },
            "logging": {"level": "info", "file": "",
                        "pattern": "[%l] %v"},
        }

        config_path = os.path.join(self.workdir, "config.json")
        with open(config_path, "w") as handle:
            json.dump(config, handle, indent=2)
        return config_path, catalog_target

    def _pump_log(self):
        """Read the server's stdout on a background thread.

        Needed for two reasons: it is where the chosen port is announced, and
        without draining the pipe the server would eventually block on a full
        stdout buffer.
        """
        for line in self.process.stdout:
            self.log_lines.append(line.rstrip())
            if self.port is None:
                match = self.PORT_PATTERN.search(line)
                if match:
                    self.port = int(match.group(1))

    def start(self, timeout=30):
        config_path, self.catalog_path = self._write_config()

        self.process = subprocess.Popen(
            [self.binary, config_path],
            cwd=self.workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
            bufsize=1,
        )

        self._log_thread = threading.Thread(target=self._pump_log, daemon=True)
        self._log_thread.start()

        # Wait for the port announcement, then for the health endpoint to
        # answer. Polling health rather than sleeping a fixed time is what
        # keeps this reliable on a loaded machine.
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError(
                    "server exited during start-up:\n" + "\n".join(self.log_lines))
            if self.port is not None:
                try:
                    response = request(self.base_url, "GET", "/api/v1/health",
                                       timeout=2)
                    if response.status == 200:
                        return
                except Exception:
                    pass
            time.sleep(0.05)

        raise RuntimeError(
            "server did not become healthy within {}s:\n{}".format(
                timeout, "\n".join(self.log_lines)))

    @property
    def base_url(self):
        return "http://127.0.0.1:{}".format(self.port)

    def stop(self, timeout=15):
        """Ask the server to shut down the way an operator would.

        SIGINT (SIGTERM on Windows, where CTRL_BREAK is awkward from Python)
        rather than kill, because a clean shutdown is part of what is being
        tested: destructors must run, the persistence worker must flush, and
        the process must exit 0.
        """
        if self.process is None or self.process.poll() is not None:
            return self.process.returncode if self.process else None

        if os.name == "nt":
            self.process.terminate()
        else:
            self.process.send_signal(signal.SIGINT)

        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise RuntimeError("server did not exit on signal")

        return self.process.returncode


# --------------------------------------------------------------------------
# The tests
# --------------------------------------------------------------------------

def test_health(base):
    section("health")
    response = request(base, "GET", "/api/v1/health")
    RESULTS.check_equal(response.status, 200, "health responds 200")
    RESULTS.check_equal(response.body.get("status"), "ok", "health reports ok")


def test_user_journey(base):
    """The six steps from the requirements, in order, over real HTTP."""
    section("end-user journey: movies -> theaters -> seats -> booking")

    # Step 1 - view all playing movies.
    response = request(base, "GET", "/api/v1/movies")
    RESULTS.check_equal(response.status, 200, "GET /movies responds 200")
    movies = response.body
    RESULTS.check(isinstance(movies, list) and len(movies) > 0,
                  "at least one movie is playing")

    movie_id = movies[0]["id"]
    RESULTS.check("title" in movies[0] and "duration_minutes" in movies[0],
                  "movie payload carries title and duration_minutes")

    # Step 2 - select a movie.
    response = request(base, "GET", "/api/v1/movies/{}".format(movie_id))
    RESULTS.check_equal(response.status, 200, "GET /movies/{id} responds 200")
    RESULTS.check_equal(response.body["id"], movie_id, "the right movie came back")

    # Step 3 - see all theaters showing that movie.
    response = request(base, "GET", "/api/v1/movies/{}/theaters".format(movie_id))
    RESULTS.check_equal(response.status, 200, "GET /movies/{id}/theaters responds 200")
    theaters = response.body
    RESULTS.check(len(theaters) > 0, "the movie is showing in at least one theater")

    theater_id = theaters[0]["id"]
    RESULTS.check("seat_capacity" in theaters[0], "theater payload carries seat_capacity")

    # Step 4 - select a theater and see the timeline.
    response = request(base, "GET",
                       "/api/v1/movies/{}/theaters/{}/showtimes".format(
                           movie_id, theater_id))
    RESULTS.check_equal(response.status, 200, "GET showtimes responds 200")
    showtimes = response.body
    RESULTS.check(len(showtimes) > 0, "the theater has at least one showtime")
    RESULTS.check(all("available_seats" in s for s in showtimes),
                  "every showtime carries an available_seats count")

    # Times must come back in order and formatted for humans.
    starts = [s["start_time"] for s in showtimes]
    RESULTS.check(starts == sorted(starts), "showtimes are ordered by start time")
    RESULTS.check(all(re.match(r"^\d\d:\d\d$", s) for s in starts),
                  "start times are formatted HH:MM")

    showtime_id = showtimes[0]["id"]

    # Step 5 - see available seats.
    response = request(base, "GET", "/api/v1/showtimes/{}/seats".format(showtime_id))
    RESULTS.check_equal(response.status, 200, "GET seats responds 200")
    seats = response.body
    available = seats["available_seats"]
    RESULTS.check(len(available) > 0, "there are seats available")
    RESULTS.check(available[0] == "a1", "seat labelling starts at a1")
    RESULTS.check_equal(len(available) + len(seats.get("booked_seats") or []),
                        seats["seat_capacity"],
                        "free + booked equals the capacity")

    # Step 6 - book seats.
    wanted = available[:2]
    response = request(base, "POST",
                       "/api/v1/showtimes/{}/bookings".format(showtime_id),
                       {"seats": wanted, "customer_name": "Smoke Tester"})
    RESULTS.check_equal(response.status, 201, "booking responds 201 Created")
    booking = response.body
    RESULTS.check_equal(sorted(booking["seats"]), sorted(wanted),
                        "the booking holds exactly the requested seats")
    RESULTS.check_equal(booking["customer_name"], "Smoke Tester",
                        "the customer name round-tripped")

    # The seats must now be gone from availability.
    response = request(base, "GET", "/api/v1/showtimes/{}/seats".format(showtime_id))
    still_available = response.body["available_seats"]
    RESULTS.check(all(seat not in still_available for seat in wanted),
                  "booked seats disappear from availability")

    # The receipt is retrievable, and cancelling gives the seats back.
    response = request(base, "GET", "/api/v1/bookings/{}".format(booking["id"]))
    RESULTS.check_equal(response.status, 200, "the booking receipt is retrievable")

    response = request(base, "DELETE", "/api/v1/bookings/{}".format(booking["id"]))
    RESULTS.check_equal(response.status, 200, "cancelling responds 200")

    response = request(base, "GET", "/api/v1/showtimes/{}/seats".format(showtime_id))
    after_cancel = response.body["available_seats"]
    RESULTS.check(all(seat in after_cancel for seat in wanted),
                  "cancelled seats return to availability")

    return showtime_id


def test_error_mapping(base, showtime_id):
    """Every ErrorCode must arrive as the documented HTTP status."""
    section("error mapping")

    response = request(base, "GET", "/api/v1/movies/does-not-exist")
    RESULTS.check_equal(response.status, 404, "unknown movie -> 404")
    RESULTS.check_equal(response.body.get("code"), "NOT_FOUND",
                        "404 body carries code NOT_FOUND")

    response = request(base, "GET", "/api/v1/showtimes/nope/seats")
    RESULTS.check_equal(response.status, 404, "unknown showtime -> 404")

    response = request(base, "POST",
                       "/api/v1/showtimes/{}/bookings".format(showtime_id),
                       {"seats": []})
    RESULTS.check_equal(response.status, 400, "empty seat list -> 400")
    RESULTS.check_equal(response.body.get("code"), "INVALID_ARGUMENT",
                        "400 body carries code INVALID_ARGUMENT")

    response = request(base, "POST",
                       "/api/v1/showtimes/{}/bookings".format(showtime_id),
                       {"seats": ["a1", "a1"]})
    RESULTS.check_equal(response.status, 400, "duplicate seat in one request -> 400")

    response = request(base, "POST",
                       "/api/v1/showtimes/{}/bookings".format(showtime_id),
                       {"seats": ["zz99"]})
    RESULTS.check_equal(response.status, 404, "unknown seat label -> 404")

    # A body that is not valid JSON is the *client's* mistake, so it must be a
    # 400. This needs a raw request because the helper always sends valid JSON.
    # Regression guard: oatpp's ParsingError derives from std::runtime_error,
    # so without an explicit catch for it a malformed body surfaces as a 500.
    raw = urllib.request.Request(
        base + "/api/v1/showtimes/{}/bookings".format(showtime_id),
        data=b"{not json",
        headers={"Content-Type": "application/json"},
        method="POST")
    try:
        with urllib.request.urlopen(raw, timeout=10) as handle:
            status = handle.status
    except urllib.error.HTTPError as error:
        status = error.code
    RESULTS.check_equal(status, 400, "a malformed JSON body -> 400, not 500")

    response = request(base, "GET", "/api/v1/no/such/route")
    RESULTS.check_equal(response.status, 404, "unrouted path -> 404")

    # Every error body must have the same shape, whoever produced it.
    RESULTS.check(isinstance(response.body, dict)
                  and "code" in response.body and "message" in response.body,
                  "framework errors use the same {code, message} envelope")


def test_no_overbooking(base, showtime_id):
    """The central requirement, exercised over the real HTTP boundary.

    Many threads request the *same* seat simultaneously. Exactly one must get
    a 201; every other must get 409 SEAT_UNAVAILABLE. This is the unit-test
    invariant re-checked end to end, so that it covers the server's threading
    model and not just the Catalog class.
    """
    section("concurrency: no over-booking across real HTTP")

    response = request(base, "GET", "/api/v1/showtimes/{}/seats".format(showtime_id))
    available = response.body["available_seats"]
    if not available:
        RESULTS.check(False, "no free seats to contend for")
        return

    contested = available[0]
    thread_count = 16
    outcomes = [None] * thread_count

    # A barrier makes every thread issue its request at the same instant.
    # Without it the first request would finish before the last one started
    # and the test would pass against a completely unsynchronised server.
    barrier = threading.Barrier(thread_count)

    def attempt(index):
        barrier.wait()
        response = request(base, "POST",
                           "/api/v1/showtimes/{}/bookings".format(showtime_id),
                           {"seats": [contested],
                            "customer_name": "racer-{}".format(index)})
        outcomes[index] = response

    threads = [threading.Thread(target=attempt, args=(i,))
               for i in range(thread_count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    created = [r for r in outcomes if r is not None and r.status == 201]
    conflicts = [r for r in outcomes if r is not None and r.status == 409]

    RESULTS.check_equal(len(created), 1,
                        "exactly one of {} concurrent requests won seat {}".format(
                            thread_count, contested))
    RESULTS.check_equal(len(conflicts), thread_count - 1,
                        "every other request was rejected with 409")
    RESULTS.check(all(r.body.get("code") == "SEAT_UNAVAILABLE" for r in conflicts),
                  "rejections carry code SEAT_UNAVAILABLE")

    # And the server's own view agrees: the seat is taken exactly once.
    response = request(base, "GET", "/api/v1/showtimes/{}/seats".format(showtime_id))
    booked = response.body.get("booked_seats") or []
    RESULTS.check_equal(booked.count(contested), 1,
                        "the seat appears exactly once in booked_seats")

    if created:
        request(base, "DELETE", "/api/v1/bookings/{}".format(created[0].body["id"]))


def test_concurrent_distinct_seats(base, showtime_id):
    """Parallel requests for *different* seats must all succeed.

    The mirror image of the previous test: it would be trivially easy to make
    over-booking impossible by rejecting everything, so this proves the lock
    does not simply serialise requests into failure.
    """
    section("concurrency: distinct seats all succeed")

    response = request(base, "GET", "/api/v1/showtimes/{}/seats".format(showtime_id))
    available = response.body["available_seats"]

    wanted = available[:8]
    if len(wanted) < 8:
        RESULTS.check(False, "not enough free seats for this test")
        return

    outcomes = [None] * len(wanted)
    barrier = threading.Barrier(len(wanted))

    def attempt(index, seat):
        barrier.wait()
        outcomes[index] = request(
            base, "POST", "/api/v1/showtimes/{}/bookings".format(showtime_id),
            {"seats": [seat], "customer_name": "parallel-{}".format(index)})

    threads = [threading.Thread(target=attempt, args=(i, seat))
               for i, seat in enumerate(wanted)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    created = [r for r in outcomes if r is not None and r.status == 201]
    RESULTS.check_equal(len(created), len(wanted),
                        "all {} distinct-seat bookings succeeded".format(len(wanted)))

    response = request(base, "GET", "/api/v1/showtimes/{}/seats".format(showtime_id))
    booked = response.body.get("booked_seats") or []
    RESULTS.check(all(seat in booked for seat in wanted),
                  "every booked seat is reflected in booked_seats")

    for booking in created:
        request(base, "DELETE", "/api/v1/bookings/{}".format(booking.body["id"]))


def test_admin_api(base):
    """The special user's routes: theaters, movies and timelines."""
    section("admin API")

    # --- theaters: append, modify, remove ---------------------------------
    response = request(base, "POST", "/api/v1/admin/theaters",
                       {"name": "Smoke Hall", "city": "Testville"})
    RESULTS.check_equal(response.status, 201, "creating a theater responds 201")
    theater = response.body
    RESULTS.check_equal(theater["seat_capacity"], 20,
                        "a theater with no capacity gets the configured default of 20")
    theater_id = theater["id"]

    response = request(base, "PUT", "/api/v1/admin/theaters/{}".format(theater_id),
                       {"name": "Smoke Hall Renamed", "city": "Testville",
                        "seat_capacity": 30})
    RESULTS.check_equal(response.status, 200, "updating a theater responds 200")
    RESULTS.check_equal(response.body["seat_capacity"], 30, "the capacity changed")

    # --- movies: append, modify -------------------------------------------
    response = request(base, "POST", "/api/v1/admin/movies",
                       {"title": "Smoke Movie", "duration_minutes": 110,
                        "language": "EN", "genre": "Test"})
    RESULTS.check_equal(response.status, 201, "creating a movie responds 201")
    movie_id = response.body["id"]

    response = request(base, "POST", "/api/v1/admin/movies",
                       {"title": "No Duration"})
    RESULTS.check_equal(response.status, 400, "a movie without a duration -> 400")

    # --- the default timeline ---------------------------------------------
    response = request(base, "POST",
                       "/api/v1/admin/movies/{}/theaters/{}/default-schedule".format(
                           movie_id, theater_id))
    RESULTS.check_equal(response.status, 201, "default schedule responds 201")

    starts = [s["start_time"] for s in response.body]
    expected = ["09:00", "11:00", "13:00", "15:00", "17:00", "19:00", "21:00"]
    RESULTS.check_equal(starts, expected,
                        "a 110-minute film gets 09:00..21:00 on whole hours")

    # --- individual time slots: append, modify, remove ---------------------
    response = request(base, "POST",
                       "/api/v1/admin/movies/{}/theaters/{}/showtimes".format(
                           movie_id, theater_id),
                       {"start_time": "10:00"})
    RESULTS.check_equal(response.status, 201, "appending a time slot responds 201")
    showtime_id = response.body["id"]

    response = request(base, "POST",
                       "/api/v1/admin/movies/{}/theaters/{}/showtimes".format(
                           movie_id, theater_id),
                       {"start_time": "10:00"})
    RESULTS.check_equal(response.status, 409, "a clashing time slot -> 409")

    response = request(base, "POST",
                       "/api/v1/admin/movies/{}/theaters/{}/showtimes".format(
                           movie_id, theater_id),
                       {"start_time": "07:00"})
    RESULTS.check_equal(response.status, 400,
                        "a slot outside the configured day -> 400")

    response = request(base, "POST",
                       "/api/v1/admin/movies/{}/theaters/{}/showtimes".format(
                           movie_id, theater_id),
                       {"start_time": "not-a-time"})
    RESULTS.check_equal(response.status, 400, "a malformed time -> 400")

    response = request(base, "PUT", "/api/v1/admin/showtimes/{}".format(showtime_id),
                       {"start_time": "12:30"})
    RESULTS.check_equal(response.status, 200, "moving a time slot responds 200")
    RESULTS.check_equal(response.body["start_time"], "12:30", "the slot moved")

    response = request(base, "DELETE",
                       "/api/v1/admin/showtimes/{}".format(showtime_id))
    RESULTS.check_equal(response.status, 200, "removing a time slot responds 200")

    # --- the admin listing differs from the public one ---------------------
    response = request(base, "POST", "/api/v1/admin/movies",
                       {"title": "Unscheduled Film", "duration_minutes": 95})
    unscheduled_id = response.body["id"]

    admin_ids = [m["id"] for m in request(base, "GET", "/api/v1/admin/movies").body]
    public_ids = [m["id"] for m in request(base, "GET", "/api/v1/movies").body]

    RESULTS.check(unscheduled_id in admin_ids,
                  "the admin listing includes an unscheduled movie")
    RESULTS.check(unscheduled_id not in public_ids,
                  "the public listing excludes an unscheduled movie")

    # --- a shrink that would drop a booked seat must be refused ------------
    seats = request(base, "GET",
                    "/api/v1/movies/{}/theaters/{}/showtimes".format(
                        movie_id, theater_id)).body
    if seats:
        target = seats[0]["id"]
        request(base, "POST", "/api/v1/showtimes/{}/bookings".format(target),
                {"seats": ["c1"], "customer_name": "Shrink Test"})

        response = request(base, "PUT",
                           "/api/v1/admin/theaters/{}".format(theater_id),
                           {"name": "Smoke Hall Renamed", "seat_capacity": 10})
        RESULTS.check_equal(response.status, 409,
                            "shrinking past a booked seat -> 409")

    # --- cleanup: removing a movie cascades --------------------------------
    response = request(base, "DELETE", "/api/v1/admin/movies/{}".format(movie_id))
    RESULTS.check_equal(response.status, 200, "removing a movie responds 200")

    response = request(base, "GET", "/api/v1/movies/{}".format(movie_id))
    RESULTS.check_equal(response.status, 404, "the removed movie is gone")

    request(base, "DELETE", "/api/v1/admin/movies/{}".format(unscheduled_id))
    request(base, "DELETE", "/api/v1/admin/theaters/{}".format(theater_id))


def test_persistence(base, server):
    """Changes must reach the catalog file, and reload must read it back."""
    section("persistence")

    response = request(base, "POST", "/api/v1/admin/movies",
                       {"title": "Persisted Film", "duration_minutes": 100})
    movie_id = response.body["id"]

    response = request(base, "POST", "/api/v1/admin/catalog/save")
    RESULTS.check_equal(response.status, 200, "an explicit save responds 200")

    with open(server.catalog_path) as handle:
        stored = json.load(handle)

    stored_ids = [m["id"] for m in stored["movies"]]
    RESULTS.check(movie_id in stored_ids, "the new movie is in the catalog file")
    RESULTS.check(all(isinstance(t["seat_capacity"], int) for t in stored["theaters"]),
                  "seat_capacity is stored as a JSON number, not a string")

    response = request(base, "POST", "/api/v1/admin/catalog/reload")
    RESULTS.check_equal(response.status, 200, "reload responds 200")

    response = request(base, "GET", "/api/v1/admin/movies")
    RESULTS.check(movie_id in [m["id"] for m in response.body],
                  "the movie survived a reload from disk")

    request(base, "DELETE", "/api/v1/admin/movies/{}".format(movie_id))


def test_clean_shutdown(server):
    """A signalled shutdown must exit 0 and flush pending work."""
    section("clean shutdown")

    request(server.base_url, "POST", "/api/v1/admin/movies",
            {"title": "Shutdown Flush", "duration_minutes": 90})

    exit_code = server.stop()
    RESULTS.check_equal(exit_code, 0, "the server exits 0 on SIGINT")

    log = "\n".join(server.log_lines)
    RESULTS.check("moviebackend stopped" in log,
                  "the shutdown ran to completion")

    with open(server.catalog_path) as handle:
        stored = json.load(handle)
    RESULTS.check(any(m["title"] == "Shutdown Flush" for m in stored["movies"]),
                  "a change made just before shutdown was flushed to disk")


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

def run_against_external(base):
    """Run the tests against a server somebody else started.

    Used by scripts/run_memcheck_server.sh, which owns the server's lifecycle
    because it has to run it under Valgrind and shut it down gracefully.

    The persistence and shutdown tests are skipped here: the first needs the
    catalog file path, which only the process that wrote the config knows, and
    the second would kill a server this process does not own.
    """
    test_health(base)
    showtime_id = test_user_journey(base)
    test_error_mapping(base, showtime_id)
    test_no_overbooking(base, showtime_id)
    test_concurrent_distinct_seats(base, showtime_id)
    test_admin_api(base)

    # The catalog save path is still worth exercising - it allocates and
    # writes, which is exactly what memcheck should watch.
    section("persistence (external server)")
    response = request(base, "POST", "/api/v1/admin/catalog/save")
    RESULTS.check_equal(response.status, 200, "an explicit save responds 200")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",
                        help="path to the moviebackend executable "
                             "(required unless --external-url is given)")
    parser.add_argument("--external-url",
                        help="run against an already-running server at this "
                             "base URL instead of starting one")
    parser.add_argument("--source-dir", default=".",
                        help="repository root, used to find config/catalog.json")
    args = parser.parse_args()

    print("MovieBackend smoke tests")

    # ---- Mode 1: drive a server somebody else is running ------------------
    if args.external_url:
        base = args.external_url.rstrip("/")
        print("  server : {} (external)".format(base))
        try:
            run_against_external(base)
        except Exception as error:
            print("\nFATAL: {}".format(error))
            RESULTS.failed += 1
            RESULTS.failures.append("harness error: {}".format(error))
        return _report()

    # ---- Mode 2: own the whole lifecycle ----------------------------------
    if not args.binary:
        print("error: either --binary or --external-url is required")
        return 2
    if not os.path.isfile(args.binary):
        print("error: no such binary: {}".format(args.binary))
        return 2

    workdir = tempfile.mkdtemp(prefix="moviebackend-smoke-")
    server = ServerProcess(args.binary, args.source_dir, workdir)

    print("  binary : {}".format(server.binary))
    print("  workdir: {}".format(workdir))

    try:
        server.start()
        print("  server : {}".format(server.base_url))

        base = server.base_url

        test_health(base)
        showtime_id = test_user_journey(base)
        test_error_mapping(base, showtime_id)
        test_no_overbooking(base, showtime_id)
        test_concurrent_distinct_seats(base, showtime_id)
        test_admin_api(base)
        test_persistence(base, server)
        test_clean_shutdown(server)

    except Exception as error:
        print("\nFATAL: {}".format(error))
        RESULTS.failed += 1
        RESULTS.failures.append("harness error: {}".format(error))
    finally:
        try:
            server.stop()
        except Exception:
            pass
        shutil.rmtree(workdir, ignore_errors=True)

    return _report()


def _report():
    """Print the summary and return the process exit code."""

    print("\n" + "=" * 60)
    print("passed: {}   failed: {}".format(RESULTS.passed, RESULTS.failed))
    if RESULTS.failures:
        print("\nfailures:")
        for failure in RESULTS.failures:
            print("  - {}".format(failure))
    print("=" * 60)

    return 0 if RESULTS.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
