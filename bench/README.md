# MiniServe Phase 7: Benchmarking & Performance Suite

This directory contains the Phase 7 benchmarking tools and automated concurrency comparison harness for MiniServe.

---

## 1. Architecture & Design

### Multi-Threaded C11 Benchmark Client (`bench/bench_client.c`)
The benchmark harness is written in C11 and designed for low-overhead, high-fidelity measurement of networked key-value store performance:
- **Lockless Latency Recording**: Each worker thread records request latencies directly into its own pre-allocated array (`worker_context_t.latencies_us`). There are zero mutex acquisitions or synchronization primitives on the critical request path.
- **High-Resolution Monotonic Clock**: Uses `clock_gettime(CLOCK_MONOTONIC)` to record elapsed time in microseconds, completely immune to system wall-clock adjustments or NTP steps.
- **Percentile Distributions**: Latencies are aggregated across all client threads and sorted using `qsort()` to compute precise distribution metrics: **min, mean, p50 (median), p75, p90, p95, p99, p99.9, and max**.
- **Buffered Line Parsing**: Uses non-blocking/buffered TCP chunk reads to prevent single-byte `recv` syscall thrashing on loopback.
- **Strict Response & Failure Accounting**: Verifies server responses against expected protocol verbs (`OK`, `VALUE`, `NOT_FOUND`). Any protocol errors (`ERR`), network read/write faults, or timeouts increment failure counts, surface prominently in the summary table, and cause `bench_client` to exit with status `EXIT_FAILURE` (`1`).
- **Graceful Shutdown**: Each worker thread negotiates clean application-level termination (`QUIT\r\n` -> `BYE\r\n`) before closing descriptors, avoiding connection resets (`RST`).

---

## 2. Building the Benchmark Client

To build the benchmark client using strict compiler flags (`-std=c11 -Wall -Wextra -Wpedantic -Werror -O2`):

```bash
make bench
```

This creates the standalone executable at `bin/bench_client`.

To build both the production server and the benchmark tool:
```bash
make release bench
```

---

## 3. CLI Usage & Options

```text
Usage: bin/bench_client [OPTIONS]

Options:
  --host <ip>        Server IPv4 address (default: 127.0.0.1)
  --port <port>      Server TCP port (default: 8080)
  -c, --clients <N>  Number of concurrent client threads (default: 10)
  -n, --requests <M> Requests per client thread (default: 1000)
  --keys <K>         Key space size (default: 1000)
  --ratio <pct>      Percentage of GET requests (0-100, default: 80)
  --val-len <bytes>  Payload value length in bytes (default: 64)
  -h, --help         Display help message and exit
```

### Example Invocations

1. **Lightweight baseline test (10 clients, 1,000 requests per client, 80% GET / 20% SET)**:
   ```bash
   ./bin/bench_client --port 8080 -c 10 -n 1000
   ```

2. **Heavy write workload (50 clients, 2,000 requests each, 100% SET, 256-byte values)**:
   ```bash
   ./bin/bench_client --port 8080 -c 50 -n 2000 --ratio 0 --val-len 256
   ```

3. **High concurrency stress test (100 clients, 5,000 requests each, 95% GET)**:
   ```bash
   ./bin/bench_client --port 8080 -c 100 -n 5000 --ratio 95
   ```

---

## 4. Automated Scalability Comparison (`run_comparison.sh`)

An automated comparison script is provided to evaluate MiniServe across different concurrency models (`threadpool` with 4 threads vs. Linux `epoll`):

```bash
chmod +x bench/run_comparison.sh
./bench/run_comparison.sh
```

You can customize port and request counts via environment variables:
```bash
PORT=18381 REQUESTS_PER_CLIENT=500 ./bench/run_comparison.sh
```

### Persistent Results Storage (`bench/results/`)
Each execution of `./bench/run_comparison.sh` automatically saves the full run configuration and formatted comparison table to:
```text
bench/results/benchmark_<YYYYMMDD_HHMMSS>.txt
```
- **No Overwriting**: Distinct UTC timestamps (with collision avoidance counters) ensure existing benchmark logs are never overwritten.
- **Saved Metadata**: Each results file records the exact timestamp, server host/port, requests per client, concurrency levels, and output table matching the terminal output.

### Illustrative Terminal Layout (Not Measured Data)

> [!IMPORTANT]
> **Illustrative Sample Numbers**: The metrics in the mock table below are **illustrative formatting examples only** to demonstrate layout and column alignment. They do not represent real, measured benchmarks from hardware. For genuine empirical metrics, inspect the files generated in `bench/results/` on your Linux environment.

```text
================================================================================
 Starting MiniServe Automated Concurrency Benchmarking
 Timestamp        : 2026-10-13 19:45:00 UTC
 Host: 127.0.0.1 | Port: 18080 | Requests/Client: 1000
 Concurrency      : 1 10 50 100
 Comparison Modes : Thread-Pool (4 threads) vs. Epoll Event-Loop
 Saving Results   : bench/results/benchmark_20261013_194500.txt
================================================================================

Mode           | Clients (C)  | Throughput(RPS)  | p50 Latency    | p99 Latency   
---------------+--------------+------------------+----------------+---------------
threadpool     | 1            | 48215.30         | 0.019 ms       | 0.042 ms      
threadpool     | 10           | 76420.12         | 0.118 ms       | 0.320 ms      
threadpool     | 50           | 51200.45         | 0.890 ms       | 2.450 ms      
threadpool     | 100          | 41800.80         | 2.120 ms       | 6.800 ms      
---------------+--------------+------------------+----------------+---------------
epoll          | 1            | 42100.20         | 0.022 ms       | 0.048 ms      
epoll          | 10           | 71800.55         | 0.125 ms       | 0.295 ms      
epoll          | 50           | 83450.90         | 0.540 ms       | 1.150 ms      
epoll          | 100          | 89120.40         | 0.980 ms       | 1.950 ms      
================================================================================
 Benchmarking complete. All test cases passed successfully.
 Saved results file : bench/results/benchmark_20261013_194500.txt
================================================================================
```

---

## 5. Architectural Performance Analysis

| Metric / Characteristic | Thread-Pool Mode (`--mode threadpool`) | Linux Epoll Mode (`--mode epoll`) |
| :--- | :--- | :--- |
| **Concurrency Mechanism** | 1 OS thread per worker (4 threads default) | Single-threaded event loop (`epoll_wait`) |
| **Low Concurrency ($C \le 10$)** | Higher potential raw throughput on multi-core systems when requests can execute across cores simultaneously | Very low latency, comparable throughput |
| **High Concurrency ($C \ge 50$)** | Throughput tends to degrade due to queue lock contention, mutex sleep/wakeups, and OS thread context switching | Throughput scales smoothly; $O(1)$ event dispatch without thread synchronization overhead |
| **Memory Footprint** | Higher (POSIX thread stack memory $\sim 2\text{MB}$ per thread, plus queue structures) | Minimal ($< 128\text{KB}$ per active connection state buffer) |
| **Failure Isolation** | Task error can impact worker thread lifecycle | Single crash terminates event loop (requires strict memory safety) |
| **Tail Latency ($p99$)** | Higher at high concurrency due to scheduler preemption and lock contention | Consistently lower at high concurrency due to cooperative non-blocking scheduling |
