# Instructions — manual testing and debugging

A hands-on companion to [README.md](README.md). The README explains *what* the
service is and *why* it is built the way it is; this file is the sequence of
commands to actually drive it, break it on purpose, and debug it.

Every command is copy-pasteable. Linux/WSL uses `curl`; the Windows section at
the end gives PowerShell equivalents.

---

## Contents

1. [Get it running in 60 seconds](#1-get-it-running-in-60-seconds)
2. [Walk the end-user journey](#2-walk-the-end-user-journey)
3. [Prove there is no over-booking](#3-prove-there-is-no-over-booking)
4. [Drive the admin API](#4-drive-the-admin-api)
5. [Exercise every error path](#5-exercise-every-error-path)
6. [Watch persistence happen](#6-watch-persistence-happen)
7. [Change the configuration](#7-change-the-configuration)
8. [Debugging recipes](#8-debugging-recipes)
9. [Windows / PowerShell equivalents](#9-windows--powershell-equivalents)
10. [Troubleshooting](#10-troubleshooting)

---

## 1. Get it running in 60 seconds

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

cd build/bin        # config/ was copied here by the build
./moviebackend
```

You should see:

```
[info] logging initialised at level 'debug'
[info] theater default capacity 20 seats, 10 per row
[info] catalog restored: 3 movies, 3 theaters, 5 showtimes, 0 bookings
[info] persistence worker started (debounce 500 ms)
[info] listening on http://0.0.0.0:8000 - press Ctrl+C to stop
```

Leave it running and open a **second terminal** for everything below.

```bash
# A shell variable saves a lot of typing.
export API=http://localhost:8000/api/v1

curl -s $API/health
# {"status":"ok","version":"1.0.0"}
```

> **Tip:** pipe anything through `python3 -m json.tool` for readable output:
> `curl -s $API/movies | python3 -m json.tool`

The seed catalog contains three movies (`m1` The Silent Protocol, `m2`
Autobahn, `m3` Deep Stack), three theaters (`t1` 20 seats, `t2` 30 seats,
`t3` 12 seats at 6 per row) and five showtimes (`s1`..`s5`). Showtime `s2`
already has `a1` and `a2` booked, so you can see a partially sold screening
immediately.

---

## 2. Walk the end-user journey

This is the six-step flow from the requirements, in order.

### Step 1 — view all playing movies

```bash
curl -s $API/movies | python3 -m json.tool
```

```json
[
    { "id": "m1", "title": "The Silent Protocol", "duration_minutes": 110, ... },
    { "id": "m2", "title": "Autobahn", ... },
    { "id": "m3", "title": "Deep Stack", ... }
]
```

### Step 2 — select a movie

```bash
curl -s $API/movies/m1 | python3 -m json.tool
```

### Step 3 — see all theaters showing it

```bash
curl -s $API/movies/m1/theaters | python3 -m json.tool
```

`m1` plays in `t1` and `t2`. Note the differing `seat_capacity` — 20 and 30.

### Step 4 — select a theater, see the timeline

```bash
curl -s $API/movies/m1/theaters/t1/showtimes | python3 -m json.tool
```

```json
[
    { "id": "s1", "start_time": "09:00", "seat_capacity": 20, "available_seats": 20 },
    { "id": "s2", "start_time": "11:00", "seat_capacity": 20, "available_seats": 18 }
]
```

`s2` shows 18 free of 20 — the two seats the seed catalog pre-booked. Times
come back sorted.

### Step 5 — see available seats

```bash
curl -s $API/showtimes/s2/seats | python3 -m json.tool
```

```json
{
    "showtime_id": "s2",
    "seat_capacity": 20,
    "seats_per_row": 10,
    "available_seats": ["a3", "a4", ..., "b10"],
    "booked_seats": ["a1", "a2"]
}
```

Both lists are returned so a seat-map UI can render the whole auditorium from
one response.

Try theater `t3`, which has a different layout (12 seats at 6 per row):

```bash
curl -s $API/showtimes/s5/seats | python3 -m json.tool
# a1..a6, b1..b6 - and note that "a7" does not exist
```

### Step 6 — book seats

```bash
curl -s -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' \
     -d '{"seats":["a1","a2","a3"],"customer_name":"Alice"}' | python3 -m json.tool
```

```json
{
    "id": "b1",
    "showtime_id": "s1",
    "seats": ["a1", "a2", "a3"],
    "customer_name": "Alice",
    "created_at": 1786426426
}
```

Confirm the seats are gone:

```bash
curl -s $API/showtimes/s1/seats | python3 -m json.tool
# available_seats no longer contains a1, a2, a3
```

Fetch the receipt, then cancel and watch the seats come back:

```bash
curl -s $API/bookings/b1 | python3 -m json.tool
curl -s -X DELETE $API/bookings/b1
curl -s $API/showtimes/s1/seats | python3 -m json.tool   # a1..a3 are free again
```

---

## 3. Prove there is no over-booking

This is the point of the whole exercise, so it is worth seeing fail-free with
your own eyes.

### The quick version — two sequential requests

```bash
curl -s -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' -d '{"seats":["a5"]}'
# {"id":"b2","seats":["a5"],...}

curl -s -w '\nHTTP %{http_code}\n' -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' -d '{"seats":["a5"]}'
# {"code":"SEAT_UNAVAILABLE","message":"seat 'a5' is already booked"}
# HTTP 409
```

### The real version — 20 simultaneous requests for one seat

```bash
SEAT=a7
for i in $(seq 1 20); do
  curl -s -o /dev/null -w '%{http_code}\n' -X POST $API/showtimes/s1/bookings \
       -H 'Content-Type: application/json' \
       -d "{\"seats\":[\"$SEAT\"],\"customer_name\":\"racer-$i\"}" &
done | sort | uniq -c
wait
```

Expected — and the only acceptable outcome:

```
      1 201
     19 409
```

**Exactly one** `201`. If you ever see two, the locking is broken.

Confirm the service agrees with itself:

```bash
curl -s $API/showtimes/s1/seats | python3 -c "
import sys, json
d = json.load(sys.stdin)
print('booked:', d['booked_seats'])
print('a7 appears', d['booked_seats'].count('a7'), 'time(s)')
print('free + booked =', len(d['available_seats']) + len(d['booked_seats']))
"
```

### All-or-nothing multi-seat bookings

`a7` is now taken. Ask for `a8` **and** `a7` together:

```bash
curl -s -w '\nHTTP %{http_code}\n' -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' -d '{"seats":["a8","a7"]}'
# HTTP 409 SEAT_UNAVAILABLE
```

Now verify `a8` was **not** quietly reserved:

```bash
curl -s $API/showtimes/s1/seats | grep -o '"a8"' || echo "a8 is NOT free - BUG!"
# "a8"   <- correct: still free
```

### The automated versions

```bash
# C++ unit tests: starting-gate threads hammering the Catalog directly
./build/bin/moviebackend_tests --gtest_filter='CatalogConcurrencyTest.*'

# Python: 16 threads released by a barrier, over real HTTP
python3 tests/smoke/run_smoke_tests.py --binary build/bin/moviebackend --source-dir .
```

---

## 4. Drive the admin API

```bash
export ADMIN=http://localhost:8000/api/v1/admin
```

### Append a theater — and see the default capacity of 20

```bash
curl -s -X POST $ADMIN/theaters -H 'Content-Type: application/json' \
     -d '{"name":"Hall Default","city":"Berlin"}' | python3 -m json.tool
# "seat_capacity": 20, "seats_per_row": 10   <- the configured default
```

With an explicit capacity:

```bash
curl -s -X POST $ADMIN/theaters -H 'Content-Type: application/json' \
     -d '{"name":"IMAX","city":"Berlin","seat_capacity":50,"seats_per_row":25}' \
     | python3 -m json.tool
```

### Append a movie

```bash
curl -s -X POST $ADMIN/movies -H 'Content-Type: application/json' \
     -d '{"title":"Dune","duration_minutes":155,"language":"EN","genre":"Sci-Fi"}' \
     | python3 -m json.tool
# note the generated id, e.g. "m4"
```

### Generate the default timeline

```bash
curl -s -X POST $ADMIN/movies/m4/theaters/t4/default-schedule | python3 -m json.tool
```

For a 155-minute film you get **09:00, 12:00, 15:00, 18:00, 21:00** — each
start is the first whole hour at or after the previous screening ends
(09:00 + 2h35 = 11:35 → 12:00).

Try it with the 110-minute `m1` to see the canonical case:

```bash
curl -s -X POST $ADMIN/movies/m1/theaters/t4/default-schedule \
  | python3 -c "import sys,json; print([s['start_time'] for s in json.load(sys.stdin)])"
# ['09:00', '11:00', '13:00', '15:00', '17:00', '19:00', '21:00']
```

> **Careful:** this is destructive. It replaces the existing timeline for that
> (movie, theater) pair and cancels its bookings.

### Append, move and remove a single time slot

```bash
# append
curl -s -X POST $ADMIN/movies/m4/theaters/t4/showtimes \
     -H 'Content-Type: application/json' -d '{"start_time":"20:00"}' | python3 -m json.tool
# note the id, e.g. "s16"

# move it (bookings are kept)
curl -s -X PUT $ADMIN/showtimes/s16 \
     -H 'Content-Type: application/json' -d '{"start_time":"19:30"}' | python3 -m json.tool

# remove it (cancels its bookings)
curl -s -X DELETE $ADMIN/showtimes/s16
```

### Modify a theater, and watch a shrink be refused

Growing a theater grows its screenings:

```bash
curl -s -X PUT $ADMIN/theaters/t1 -H 'Content-Type: application/json' \
     -d '{"name":"Cinema Central","city":"Munich","seat_capacity":40}' | python3 -m json.tool

curl -s $API/showtimes/s1/seats | python3 -c "import sys,json; print(len(json.load(sys.stdin)['available_seats']), 'free')"
```

Now book a high seat and try to shrink past it:

```bash
curl -s -X POST $API/showtimes/s1/bookings -H 'Content-Type: application/json' \
     -d '{"seats":["d10"]}' > /dev/null

curl -s -w '\nHTTP %{http_code}\n' -X PUT $ADMIN/theaters/t1 \
     -H 'Content-Type: application/json' \
     -d '{"name":"Cinema Central","seat_capacity":10}'
# {"code":"CONFLICT","message":"cannot change layout ... seat 'd10' is booked ..."}
# HTTP 409
```

Nothing changed — the whole update was rolled back:

```bash
curl -s $ADMIN/theaters | python3 -c "
import sys,json
print([t for t in json.load(sys.stdin) if t['id']=='t1'])"
# still seat_capacity 40
```

### The admin listing differs from the public one

An unscheduled movie is invisible to end users but visible to the
administrator:

```bash
curl -s -X POST $ADMIN/movies -H 'Content-Type: application/json' \
     -d '{"title":"Unscheduled","duration_minutes":90}' | python3 -m json.tool

curl -s $ADMIN/movies | python3 -c "import sys,json; print('admin :',[m['id'] for m in json.load(sys.stdin)])"
curl -s $API/movies   | python3 -c "import sys,json; print('public:',[m['id'] for m in json.load(sys.stdin)])"
```

### Cascading deletes

```bash
curl -s -X DELETE $ADMIN/movies/m4
# {"status":"removed","detail":"movie m4, its showtimes and their bookings"}

curl -s -w '\nHTTP %{http_code}\n' $API/movies/m4
# HTTP 404
```

---

## 5. Exercise every error path

Each of these should produce the documented status **and** the standard
`{code, message}` envelope.

```bash
# 404 - unknown entities
curl -s -w ' [%{http_code}]\n' $API/movies/nope
curl -s -w ' [%{http_code}]\n' $API/showtimes/nope/seats
curl -s -w ' [%{http_code}]\n' $API/bookings/nope

# 404 - unroutable path (framework error, same envelope)
curl -s -w ' [%{http_code}]\n' $API/no/such/route

# 400 - empty seat list
curl -s -w ' [%{http_code}]\n' -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' -d '{"seats":[]}'

# 400 - the same seat twice in one request
curl -s -w ' [%{http_code}]\n' -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' -d '{"seats":["b1","b1"]}'

# 400 - "b1" and "B1" are the same seat, so this is also a duplicate
curl -s -w ' [%{http_code}]\n' -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' -d '{"seats":["b1","B1"]}'

# 404 - a seat label that does not exist in this layout
curl -s -w ' [%{http_code}]\n' -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' -d '{"seats":["z99"]}'

# 400 - missing body
curl -s -w ' [%{http_code}]\n' -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' -d '{}'

# 400 - malformed JSON
curl -s -w ' [%{http_code}]\n' -X POST $API/showtimes/s1/bookings \
     -H 'Content-Type: application/json' -d '{not json'

# 400 - a movie with no duration
curl -s -w ' [%{http_code}]\n' -X POST $ADMIN/movies \
     -H 'Content-Type: application/json' -d '{"title":"No Duration"}'

# 400 - a time slot outside the configured day (09:00..21:00)
curl -s -w ' [%{http_code}]\n' -X POST $ADMIN/movies/m1/theaters/t1/showtimes \
     -H 'Content-Type: application/json' -d '{"start_time":"07:00"}'

# 400 - a malformed time
curl -s -w ' [%{http_code}]\n' -X POST $ADMIN/movies/m1/theaters/t1/showtimes \
     -H 'Content-Type: application/json' -d '{"start_time":"7am"}'

# 409 - two screenings in one hall at the same minute
curl -s -w ' [%{http_code}]\n' -X POST $ADMIN/movies/m2/theaters/t1/showtimes \
     -H 'Content-Type: application/json' -d '{"start_time":"09:00"}'
```

---

## 6. Watch persistence happen

The catalog file is rewritten by a background thread a short time after any
change (500 ms by default).

```bash
# In a third terminal, watch the file:
watch -n 1 'python3 -c "
import json
d = json.load(open(\"build/bin/config/catalog.json\"))
print(\"movies:\", len(d[\"movies\"]), \" theaters:\", len(d[\"theaters\"]))
print(\"showtimes:\", len(d[\"showtimes\"]), \" bookings:\", len(d[\"bookings\"]))
"'
```

Now make a booking in the other terminal and watch the counts change within a
second.

### Force a synchronous checkpoint

```bash
curl -s -X POST $ADMIN/catalog/save
# {"status":"saved","detail":"the catalog was written to its JSON file"}
```

Unlike autosave, this responds only once the bytes are on disk.

### Edit the file by hand and reload

```bash
# Stop the server first, or your edit will be overwritten by the next autosave.
# Then, with the server running again:
curl -s -X POST $ADMIN/catalog/reload
```

> **Careful:** reload is destructive — bookings made since the last save are
> lost. If the file is invalid you get a `400` and the live catalog is left
> exactly as it was, which you can verify:

```bash
echo '{ "movies": [ {"id":"m1"} ] }' > /tmp/broken.json   # no title
# point catalog.path at it, restart, then:
curl -s -w ' [%{http_code}]\n' -X POST $ADMIN/catalog/reload
# {"code":"INVALID_ARGUMENT","message":"...: field 'movies[0].title' is required"} [400]
curl -s $API/movies    # unchanged
```

### Confirm a clean shutdown flushes

```bash
curl -s -X POST $ADMIN/movies -H 'Content-Type: application/json' \
     -d '{"title":"Flush Me","duration_minutes":90}'

# immediately press Ctrl+C in the server terminal, then:
grep -c "Flush Me" build/bin/config/catalog.json      # 1
```

The shutdown log shows the final flush:

```
[info] REST server: stop requested
[info] shutting down
[debug] catalog saved to 'config/catalog.json' (3542 bytes)
[info] persistence worker stopped after 4 save(s)
[info] moviebackend stopped
```

---

## 7. Change the configuration

Edit `build/bin/config/config.json` (or pass your own file as the first
argument) and restart.

### A different default seat capacity

```json
{ "theater_defaults": { "seat_capacity": 6, "seats_per_row": 3 } }
```

Any theater created without an explicit capacity now gets 6 seats labelled
`a1`..`a3`, `b1`..`b3`.

### A different screening day

```json
{ "schedule": { "day_start": "12:00", "day_end": "16:00" } }
```

A 90-minute film's default timeline becomes `12:00, 14:00, 16:00`.

### Verbose logging

```json
{ "logging": { "level": "trace", "file": "moviebackend.log" } }
```

Output goes to both the console and the file. Note that a **Release** build has
everything below *error* compiled out, so `"level": "trace"` will show nothing
there — that is the compile-time filter doing its job. Use a Debug build when
you need detail.

### An ephemeral port

```json
{ "server": { "port": 0 } }
```

The OS picks a free port; the server logs which one it got. This is what the
smoke tests use so they never clash with a server you already have running.

### Disable autosave

```json
{ "catalog": { "autosave": false } }
```

The server warns at start-up, and the file is only written when you call
`POST /admin/catalog/save`.

---

## 8. Debugging recipes

### Turn up the logs

```bash
# Server: set logging.level to "trace" in config.json (Debug build).
# Tests:
MOVIEBACKEND_TEST_LOG_LEVEL=debug ./build/bin/moviebackend_tests
```

Every log line carries a thread id, which is what makes a concurrency problem
readable:

```
[2026-08-11 05:34:42.884] [debug] [thread 243094] listing playing movies
[2026-08-11 05:34:42.953] [info]  [thread 242375] booking b1 confirmed: 2 seat(s)
```

### Run one test under GDB

```bash
gdb --args ./build/bin/moviebackend_tests \
    --gtest_filter='CatalogConcurrencyTest.OnlyOneThreadCanWinASingleSeat'
(gdb) break moviebackend::Catalog::bookSeats
(gdb) run
(gdb) info threads          # see every worker thread
(gdb) thread apply all bt   # the classic deadlock diagnostic
```

### Attach to the running server

```bash
gdb -p $(pgrep -x moviebackend)
(gdb) thread apply all bt
```

### Hunt for data races with ThreadSanitizer

Valgrind's memcheck does not detect races; TSan does. This is the tool to reach
for if you suspect the locking:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
cmake --build build-tsan --target moviebackend_tests -j$(nproc)
./build-tsan/bin/moviebackend_tests --gtest_filter='CatalogConcurrencyTest.*'
```

A clean run is meaningful evidence: those tests deliberately maximise
contention.

### Check for leaks

```bash
cmake --build build --target memcheck-tests     # the logic
cmake --build build --target memcheck-server    # a real request cycle
```

To investigate one binary yourself:

```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
         --suppressions=tests/valgrind.supp \
         ./build/bin/moviebackend_tests
```

### See exactly what is on the wire

```bash
curl -v $API/movies                 # request and response headers
curl -s -D - -o /dev/null $API/movies   # headers only
```

Or watch the traffic:

```bash
sudo tcpdump -i lo -A 'tcp port 8000'
```

### Reproduce a "port already in use"

```bash
./moviebackend &     # first instance
./moviebackend       # second
# [critical] fatal: cannot listen on 0.0.0.0:8000 - ... (INTERNAL)
```

### Start from a clean catalog

```bash
rm build/bin/config/catalog.json
./moviebackend
# [warning] catalog file 'config/catalog.json' does not exist - starting with an empty catalog
```

A missing file is not an error; a *corrupt* one is, and the server refuses to
start rather than silently discard your data.

---

## 9. Windows / PowerShell equivalents

`curl` in PowerShell is an alias for `Invoke-WebRequest`, which behaves
differently. Use `curl.exe` (shipped with Windows 10+) for the commands above
verbatim, or use these PowerShell-native versions.

```powershell
$API   = "http://localhost:8000/api/v1"
$ADMIN = "$API/admin"

# GET
Invoke-RestMethod "$API/movies" | ConvertTo-Json -Depth 5
Invoke-RestMethod "$API/showtimes/s1/seats" | ConvertTo-Json -Depth 5

# POST a booking
$body = @{ seats = @("a1","a2"); customer_name = "Alice" } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri "$API/showtimes/s1/bookings" `
                  -ContentType "application/json" -Body $body

# An expected failure - PowerShell throws on 4xx, so catch it
try {
    Invoke-RestMethod -Method Post -Uri "$API/showtimes/s1/bookings" `
                      -ContentType "application/json" -Body $body
} catch {
    $_.Exception.Response.StatusCode.value__      # 409
    $_.ErrorDetails.Message                       # the {code, message} body
}

# Admin: create a theater
$t = @{ name = "Hall 3"; city = "Berlin" } | ConvertTo-Json
Invoke-RestMethod -Method Post -Uri "$ADMIN/theaters" `
                  -ContentType "application/json" -Body $t
```

### 20 concurrent bookings in PowerShell

```powershell
$jobs = 1..20 | ForEach-Object {
    Start-Job -ScriptBlock {
        $b = @{ seats = @("a7") } | ConvertTo-Json
        try {
            Invoke-WebRequest -Method Post `
                -Uri "http://localhost:8000/api/v1/showtimes/s1/bookings" `
                -ContentType "application/json" -Body $b -UseBasicParsing |
                Select-Object -ExpandProperty StatusCode
        } catch { $_.Exception.Response.StatusCode.value__ }
    }
}
$jobs | Wait-Job | Receive-Job | Group-Object | Select-Object Count, Name
# Expect exactly one 201 and nineteen 409s.
```

### Debugging on Windows

* **Visual Studio**: open the generated `build\MovieBackend.sln`, set
  `moviebackend` as the start-up project, and set its working directory to
  `build\bin\Debug` so `config/` is found.
* **Leak detection**: Valgrind does not exist on Windows. Use
  `-DCMAKE_CXX_FLAGS="/fsanitize=address"` for AddressSanitizer, or enable
  Application Verifier for the executable.
* Stop the server with **Ctrl+C** so destructors run and the catalog is
  flushed; closing the console window is a hard kill.

---

## 10. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `cannot listen on 0.0.0.0:8000` | Another instance is already running. `pkill -x moviebackend`, or set `server.port` to something else (or `0`). |
| `cannot load configuration` | The working directory is wrong. Run from `build/bin`, or pass an absolute path: `./moviebackend /abs/path/config.json`. |
| `catalog file ... does not exist` (warning) | Not an error — the service starts with an empty catalog. Copy `config/catalog.json` next to the binary if you want the seed data. |
| Server refuses to start after a hand edit | The catalog is invalid; the log names the offending field, e.g. `field 'movies[0].title' is required`. Fix it or delete the file. |
| Trace/debug logs missing | You are on a Release build, where they are compiled out. Rebuild with `-DCMAKE_BUILD_TYPE=Debug`. |
| Smoke tests: "server did not become healthy" | The binary path is wrong, or the build is stale. Rebuild and check `--binary` points at the real executable. |
| CMake configure fails on the first run | It needs network access to fetch oat++. Later builds are offline. |
| `No mapping for HTTP-method` | The URL is wrong. Check the method and path against the table in [README.md](README.md#rest-api-reference). |
| A booking "disappeared" | Something destructive ran: `POST /admin/catalog/reload`, a `default-schedule` regeneration, or the deletion of the parent movie/theater/showtime. All are documented as destructive. |
| Two `201`s in the over-booking test | A genuine locking bug. Run the C++ concurrency tests and the TSan build; see [Debugging recipes](#8-debugging-recipes). |
