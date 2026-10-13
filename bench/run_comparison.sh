#!/usr/bin/env bash
# ==============================================================================
# MiniServe Phase 7 Automated Concurrency & Performance Comparison Suite
# ==============================================================================
# Compares MiniServe's architectural concurrency modes:
#   1. Thread-Pool Mode (--mode threadpool --threads 4)
#   2. Linux Epoll Event-Loop Mode (--mode epoll)
# Across concurrent connection scales (1, 10, 50, 100 concurrent clients).
#
# Saves run configuration and full benchmark comparison results to:
#   bench/results/benchmark_<timestamp>.txt
# ==============================================================================

set -euo pipefail

PORT=${PORT:-18080}
HOST="127.0.0.1"
REQUESTS_PER_CLIENT=${REQUESTS_PER_CLIENT:-1000}
CONCURRENCY_LEVELS=(1 10 50 100)
SERVER_PID=""
OVERALL_FAILED=0

# Ensure project root directory is working directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

# Temporary server log
TMP_LOG=$(mktemp)

# Setup persistent results file under bench/results/ without overwriting previous runs
RESULTS_DIR="${PROJECT_ROOT}/bench/results"
mkdir -p "${RESULTS_DIR}"

RUN_DATE=$(date -u +"%Y-%m-%d %H:%M:%S UTC")
RUN_TIMESTAMP=$(date -u +"%Y%m%d_%H%M%S")

if [ -z "${RESULTS_FILE:-}" ]; then
    RESULTS_FILE="${RESULTS_DIR}/benchmark_${RUN_TIMESTAMP}.txt"
    file_idx=1
    while [ -e "${RESULTS_FILE}" ]; do
        RESULTS_FILE="${RESULTS_DIR}/benchmark_${RUN_TIMESTAMP}_${file_idx}.txt"
        file_idx=$((file_idx + 1))
    done
else
    # If custom RESULTS_FILE was provided, ensure parent directory exists
    mkdir -p "$(dirname "${RESULTS_FILE}")"
fi

cleanup() {
    local exit_code=$?
    trap - EXIT INT TERM HUP

    # Clean up temporary logs (preserve persistent RESULTS_FILE)
    if [ -n "${TMP_LOG:-}" ] && [ -f "${TMP_LOG}" ]; then
        rm -f "${TMP_LOG}" 2>/dev/null || true
    fi

    # Terminate background server if running
    if [ -n "${SERVER_PID:-}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        local count=0
        while kill -0 "${SERVER_PID}" 2>/dev/null && [ "${count}" -lt 20 ]; do
            sleep 0.1
            count=$((count + 1))
        done
        if kill -0 "${SERVER_PID}" 2>/dev/null; then
            kill -KILL "${SERVER_PID}" 2>/dev/null || true
        fi
        wait "${SERVER_PID}" 2>/dev/null || true
        SERVER_PID=""
    fi

    exit "${exit_code}"
}
trap cleanup EXIT INT TERM HUP

# Ensure binaries are built
if [ ! -x "bin/miniserve" ] || [ ! -x "bin/bench_client" ]; then
    echo "Building MiniServe release binary and Phase 7 benchmark client..."
    make release bench
fi

wait_for_server() {
    local target_port=$1
    local max_wait=30
    local count=0
    while ! (exec 3<>/dev/tcp/127.0.0.1/"${target_port}") 2>/dev/null; do
        sleep 0.1
        count=$((count + 1))
        if [ "${count}" -ge "${max_wait}" ]; then
            echo "Error: Server failed to start on port ${target_port} within 3 seconds." >&2
            return 1
        fi
    done
    exec 3<&-
    exec 3>&-
    return 0
}

log_both() {
    echo "$@" | tee -a "${RESULTS_FILE}"
}

printf_both() {
    local fmt=$1
    shift
    printf "${fmt}" "$@" | tee -a "${RESULTS_FILE}"
}

# Write initial configuration header to persistent results file
cat <<EOF > "${RESULTS_FILE}"
================================================================================
 MiniServe Benchmark Comparison Results
 Timestamp           : ${RUN_DATE}
 Host                : ${HOST}
 Port                : ${PORT}
 Requests Per Client : ${REQUESTS_PER_CLIENT}
 Concurrency Levels  : ${CONCURRENCY_LEVELS[*]}
 Comparison Modes    : Thread-Pool (4 threads) vs. Epoll Event-Loop
 Results File        : ${RESULTS_FILE}
================================================================================

EOF

echo "================================================================================"
echo " Starting MiniServe Automated Concurrency Benchmarking"
echo " Timestamp        : ${RUN_DATE}"
echo " Host: ${HOST} | Port: ${PORT} | Requests/Client: ${REQUESTS_PER_CLIENT}"
echo " Concurrency      : ${CONCURRENCY_LEVELS[*]}"
echo " Comparison Modes : Thread-Pool (4 threads) vs. Epoll Event-Loop"
echo " Saving Results   : ${RESULTS_FILE}"
echo "================================================================================"
echo ""

# Table header and divider (emitted to both terminal and results file)
printf_both "%-14s | %-12s | %-16s | %-14s | %-14s\n" "Mode" "Clients (C)" "Throughput(RPS)" "p50 Latency" "p99 Latency"
log_both "---------------+--------------+------------------+----------------+---------------"

run_benchmark_case() {
    local mode=$1
    local extra_args=$2
    local concurrency=$3

    # Launch server
    ./bin/miniserve --port "${PORT}" --mode "${mode}" ${extra_args} > "${TMP_LOG}" 2>&1 &
    SERVER_PID=$!

    # Wait until server socket is ready to accept connections
    if ! wait_for_server "${PORT}"; then
        echo "Failed to start server for mode=${mode}" >&2
        cat "${TMP_LOG}" >&2 2>/dev/null || true
        OVERALL_FAILED=1
        printf_both "%-14s | %-12d | %-16s | %-14s | %-14s\n" "${mode}" "${concurrency}" "FAILED" "FAILED" "FAILED"
        if [ -n "${SERVER_PID}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
            kill -KILL "${SERVER_PID}" 2>/dev/null || true
            wait "${SERVER_PID}" 2>/dev/null || true
            SERVER_PID=""
        fi
        return 0
    fi

    # Execute benchmark client and capture both stdout and exit status
    set +e
    local bench_out
    bench_out=$(./bin/bench_client --host "${HOST}" --port "${PORT}" -c "${concurrency}" -n "${REQUESTS_PER_CLIENT}" 2>&1)
    local bench_status=$?
    set -e

    # Parse metrics with exact word matching so p99 does not match p99.9
    local rps
    local p50
    local p99
    local failed_reqs
    rps=$(echo "${bench_out}" | awk '$1 == "Throughput" && $2 == ":" {printf "%.2f", $3}')
    p50=$(echo "${bench_out}" | awk '$1 == "p50" && $2 == ":" {print $3, $4}')
    p99=$(echo "${bench_out}" | awk '$1 == "p99" && $2 == ":" {print $3, $4}')
    failed_reqs=$(echo "${bench_out}" | awk '/Failed \/ Timed Out/ {print $NF}')

    if [ "${bench_status}" -ne 0 ] || [ -n "${failed_reqs}" -a "${failed_reqs:-0}" -gt 0 ] || [ -z "${rps}" ] || [ -z "${p50}" ] || [ -z "${p99}" ]; then
        OVERALL_FAILED=1
        rps="FAILED"
        p50="FAILED"
        p99="FAILED"
    fi

    printf_both "%-14s | %-12d | %-16s | %-14s | %-14s\n" "${mode}" "${concurrency}" "${rps}" "${p50}" "${p99}"

    # Terminate server gracefully
    if [ -n "${SERVER_PID}" ] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        local count=0
        while kill -0 "${SERVER_PID}" 2>/dev/null && [ "${count}" -lt 20 ]; do
            sleep 0.1
            count=$((count + 1))
        done
        if kill -0 "${SERVER_PID}" 2>/dev/null; then
            kill -KILL "${SERVER_PID}" 2>/dev/null || true
        fi
        wait "${SERVER_PID}" 2>/dev/null || true
        SERVER_PID=""
    fi
    sleep 0.2
}

# 1. Thread-Pool Mode (4 worker threads)
for c in "${CONCURRENCY_LEVELS[@]}"; do
    run_benchmark_case "threadpool" "--threads 4" "${c}"
done

log_both "---------------+--------------+------------------+----------------+---------------"

# 2. Linux Epoll Event-Loop Mode
for c in "${CONCURRENCY_LEVELS[@]}"; do
    run_benchmark_case "epoll" "" "${c}"
done

log_both "================================================================================"
if [ "${OVERALL_FAILED}" -ne 0 ]; then
    log_both " Benchmarking finished with ERRORS (one or more test cases failed)."
    log_both " Saved results file : ${RESULTS_FILE}"
    log_both "================================================================================"
    exit 1
else
    log_both " Benchmarking complete. All test cases passed successfully."
    log_both " Saved results file : ${RESULTS_FILE}"
    log_both "================================================================================"
    exit 0
fi
