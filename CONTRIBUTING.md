# Contributing to MiniServe

Thank you for your interest in MiniServe! This document explains how to set up
your environment, run the test suite, and submit changes.

---

## Environment Requirements

| Requirement | Minimum |
|---|---|
| OS | Linux (kernel ≥ 2.6 for epoll) |
| Compiler | GCC ≥ 10 or Clang ≥ 12 (C11 support required) |
| Build tool | GNU Make |
| Optional | Valgrind, ThreadSanitizer (`-fsanitize=thread`) |

> **Note**: MiniServe uses Linux-specific APIs (`epoll`, `pipe2`, `O_CLOEXEC`).
> It will not compile or run on macOS or Windows natively.

---

## Build from Source

```bash
# Clone the repository
git clone https://github.com/keerthana7-create/miniserve.git
cd miniserve

# Build the production server
make

# Build the benchmark client as well
make bench

# Build in debug mode (no optimisation, assertions active)
make debug
```

All targets use strict C11 flags:
```
-std=c11 -Wall -Wextra -Wpedantic -Werror
```
**Any compiler warning is treated as an error.** Your code must compile cleanly.

---

## Running the Server

```bash
# Thread-pool mode (default), port 8080
./bin/miniserve -p 8080

# Epoll mode with a 10,000-key LRU cap
./bin/miniserve -p 8080 -m epoll -K 10000

# Full help
./bin/miniserve --help
```

---

## Test Suite

```bash
# Run all Phase 1–8 unit and integration tests
make test

# Run with ThreadSanitizer instrumentation (detects data races)
make tsan

# Run Valgrind memory analysis
make valgrind
```

All assertions must pass. `make test` exits non-zero on any failure.

---

## Benchmarking

```bash
# Build the benchmark client
make bench

# Compare threadpool vs epoll across concurrency levels
./bench/run_comparison.sh

# Custom port and request volume
PORT=18381 REQUESTS_PER_CLIENT=500 ./bench/run_comparison.sh
```

Results are saved automatically to `bench/results/benchmark_<timestamp>.txt`.

---

## Project Layout

```
miniserve/
├── src/                  # Production C source files
│   ├── main.c            # Entry point, CLI parsing
│   ├── server.c          # Thread-pool and epoll server engines
│   ├── hashtable.c       # Lock-free hash table, LRU, TTL
│   ├── protocol.c        # Request parser and command dispatch
│   ├── threadpool.c      # Bounded FIFO thread pool
│   ├── logger.c          # Thread-safe levelled logger
│   └── util.c            # Shared utilities
├── include/              # Public header files
├── tests/                # Unit and integration test harnesses
│   ├── test_phase1.c     # CLI, logger
│   ├── test_hashtable.c  # Hash table correctness
│   ├── test_protocol.c   # Protocol parsing
│   ├── test_threadpool.c # Thread pool correctness
│   ├── test_server.c     # TCP integration tests
│   ├── test_robustness.c # Signal handling, idle timeout
│   ├── test_epoll.c      # Epoll event loop tests
│   └── test_phase8.c     # TTL expiration, LRU eviction
├── bench/
│   ├── bench_client.c    # Multi-threaded C11 benchmark harness
│   ├── run_comparison.sh # Automated threadpool vs. epoll comparison
│   └── results/          # Auto-generated benchmark result files
├── Makefile
├── README.md
├── CONTRIBUTING.md       # This file
└── LICENSE
```

---

## Code Style

- **C11 standard only.** No compiler extensions, no GNU-isms in headers.
- **`#define` guards** on every header: `#ifndef MINISERVE_<NAME>_H`.
- **Error handling**: every syscall return value must be checked.
- **No global mutable state** outside of the designated server config struct and atomic counters.
- **Lock order** for the storage engine: `table rwlock → lru_lock`. Never invert.

---

## Submitting Changes

1. Fork the repository and create a feature branch:
   ```bash
   git checkout -b feat/your-feature-name
   ```
2. Make changes, ensuring `make test` and `make tsan` pass.
3. Commit with a clear, imperative message:
   ```
   Add persistent AOF log for crash recovery
   ```
4. Open a Pull Request against `main` with a description of what and why.

---

## Reporting Bugs

Open a GitHub Issue with:
- OS and kernel version (`uname -a`)
- Compiler version (`gcc --version`)
- Exact steps to reproduce
- Expected vs. actual behaviour
- Any assertion or sanitizer output
