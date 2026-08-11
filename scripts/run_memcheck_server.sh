#!/usr/bin/env bash
#
# Runs the MovieBackend server under Valgrind memcheck while the Python smoke
# tests drive real traffic through it, then shuts it down cleanly and reports
# whether memcheck found anything.
#
# Invoked by the CMake target:
#
#     cmake --build build --target memcheck-server
#
# Why a script and not a plain custom command
# -------------------------------------------
# The interesting memory bugs in a service like this are not in the pure
# logic - the unit tests under memcheck already cover that. They are in the
# request lifecycle: connection buffers, DTO allocation, the objects a worker
# thread builds and drops per request. Catching those needs a *live* server
# with *real* traffic, which means: start it, wait for it to be ready, drive
# it, stop it gracefully, and only then read the verdict.
#
# The graceful stop matters more than it looks. Killing the process with
# SIGKILL would mean no destructor ever ran, and memcheck would report the
# entire live heap as leaked. SIGINT lets main() unwind properly, so anything
# memcheck still reports is a real leak.
#
# Environment variables (set by cmake/Valgrind.cmake):
#   MOVIEBACKEND_BINARY      path to the server executable
#   MOVIEBACKEND_VALGRIND    path to the valgrind executable
#   MOVIEBACKEND_SOURCE_DIR  repository root
#   MOVIEBACKEND_BUILD_DIR   build tree

set -o errexit
set -o nounset
set -o pipefail

: "${MOVIEBACKEND_BINARY:?MOVIEBACKEND_BINARY is not set}"
: "${MOVIEBACKEND_VALGRIND:?MOVIEBACKEND_VALGRIND is not set}"
: "${MOVIEBACKEND_SOURCE_DIR:?MOVIEBACKEND_SOURCE_DIR is not set}"
: "${MOVIEBACKEND_BUILD_DIR:?MOVIEBACKEND_BUILD_DIR is not set}"

PORT="${MOVIEBACKEND_MEMCHECK_PORT:-18642}"
WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/moviebackend-memcheck-XXXXXX")"
LOGFILE="${WORKDIR}/valgrind.log"
SERVER_LOG="${WORKDIR}/server.log"

cleanup() {
    # Never leave a server (or a valgrind wrapper) behind, whatever happened.
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -KILL "${SERVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "=============================================================="
echo " MovieBackend - server under Valgrind memcheck"
echo "=============================================================="
echo "  binary   : ${MOVIEBACKEND_BINARY}"
echo "  workdir  : ${WORKDIR}"
echo "  port     : ${PORT}"
echo

# ---------------------------------------------------------------------------
# A private config and catalog, so the run never touches the checked-in files.
#
# A fixed port is used rather than 0: the port has to be known up front so the
# readiness probe below can poll it, and running under valgrind is slow enough
# that parsing it out of the log adds avoidable flakiness.
# ---------------------------------------------------------------------------
cp "${MOVIEBACKEND_SOURCE_DIR}/config/catalog.json" "${WORKDIR}/catalog.json"

cat > "${WORKDIR}/config.json" <<EOF
{
  "server":  { "host": "127.0.0.1", "port": ${PORT} },
  "theater_defaults": { "seat_capacity": 20, "seats_per_row": 10 },
  "schedule": { "day_start": "09:00", "day_end": "21:00" },
  "catalog": {
    "path": "${WORKDIR}/catalog.json",
    "autosave": true,
    "autosave_debounce_ms": 50
  },
  "logging": { "level": "info", "file": "", "pattern": "[%l] %v" }
}
EOF

# ---------------------------------------------------------------------------
# Start the server under memcheck.
# ---------------------------------------------------------------------------
echo "--- starting the server under memcheck (this is slow: 20-50x) ---"

"${MOVIEBACKEND_VALGRIND}" \
    --tool=memcheck \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --track-origins=yes \
    --errors-for-leak-kinds=definite \
    --error-exitcode=99 \
    --child-silent-after-fork=yes \
    --suppressions="${MOVIEBACKEND_SOURCE_DIR}/tests/valgrind.supp" \
    --log-file="${LOGFILE}" \
    "${MOVIEBACKEND_BINARY}" "${WORKDIR}/config.json" \
    > "${SERVER_LOG}" 2>&1 &

SERVER_PID=$!

# ---------------------------------------------------------------------------
# Wait for readiness by polling /api/v1/health.
#
# The timeout is generous because everything runs 20-50x slower under
# memcheck; a fixed sleep would be either wasteful or flaky.
# ---------------------------------------------------------------------------
echo "--- waiting for the server to become healthy ---"

READY=0
for _ in $(seq 1 120); do
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
        echo "ERROR: the server exited during start-up."
        echo "--- server log ---";   cat "${SERVER_LOG}"
        echo "--- valgrind log ---"; cat "${LOGFILE}"
        exit 1
    fi

    if curl --silent --fail --max-time 2 \
            "http://127.0.0.1:${PORT}/api/v1/health" > /dev/null 2>&1; then
        READY=1
        break
    fi
    sleep 1
done

if [[ "${READY}" -ne 1 ]]; then
    echo "ERROR: the server did not become healthy in time."
    echo "--- server log ---";   cat "${SERVER_LOG}"
    exit 1
fi

echo "--- server is up; driving traffic with the smoke tests ---"

# ---------------------------------------------------------------------------
# Drive real traffic.
#
# The smoke suite normally starts its own server; here it must talk to the one
# already running under memcheck, so it is invoked with --external-url. Its
# shutdown test is skipped for the same reason - this script owns the
# lifecycle.
# ---------------------------------------------------------------------------
SMOKE_STATUS=0
python3 "${MOVIEBACKEND_SOURCE_DIR}/tests/smoke/run_smoke_tests.py" \
    --external-url "http://127.0.0.1:${PORT}" \
    --source-dir "${MOVIEBACKEND_SOURCE_DIR}" || SMOKE_STATUS=$?

echo
echo "--- smoke tests finished (status ${SMOKE_STATUS}); stopping the server ---"

# ---------------------------------------------------------------------------
# Graceful shutdown, so destructors actually run.
# ---------------------------------------------------------------------------
kill -INT "${SERVER_PID}" 2>/dev/null || true

SERVER_STATUS=0
for _ in $(seq 1 120); do
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
        break
    fi
    sleep 1
done

if kill -0 "${SERVER_PID}" 2>/dev/null; then
    echo "ERROR: the server ignored SIGINT; killing it."
    echo "       (memcheck output below will be meaningless - no destructor ran)"
    kill -KILL "${SERVER_PID}" 2>/dev/null || true
    SERVER_STATUS=1
fi

wait "${SERVER_PID}" 2>/dev/null || SERVER_STATUS=$?
unset SERVER_PID

# ---------------------------------------------------------------------------
# Report.
# ---------------------------------------------------------------------------
echo
echo "=============================================================="
echo " memcheck summary"
echo "=============================================================="
sed -n '/LEAK SUMMARY/,$p' "${LOGFILE}" 2>/dev/null || true
grep -E "ERROR SUMMARY|definitely lost|indirectly lost" "${LOGFILE}" || true
echo
echo "full valgrind log: ${LOGFILE}"
echo "full server log  : ${SERVER_LOG}"

# valgrind was told --error-exitcode=99, so 99 means it found something.
if [[ "${SERVER_STATUS}" -eq 99 ]]; then
    echo
    echo "RESULT: FAILED - memcheck reported errors or definite leaks."
    exit 1
fi

if [[ "${SMOKE_STATUS}" -ne 0 ]]; then
    echo
    echo "RESULT: FAILED - the smoke tests failed while running under memcheck."
    exit 1
fi

if [[ "${SERVER_STATUS}" -ne 0 ]]; then
    echo
    echo "RESULT: FAILED - the server exited with status ${SERVER_STATUS}."
    exit 1
fi

echo
echo "RESULT: PASSED - no memcheck errors, no definite leaks."
