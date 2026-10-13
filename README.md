# MiniServe

A high-performance, concurrent in-memory TCP key-value server written in strict **C11** for POSIX / Linux environments.

MiniServe supports dual concurrency architectures (**Thread-Pool** and Linux **Epoll Event-Loop**), a thread-safe dynamic hash table with optional **LRU eviction** and **millisecond-precision TTL expiration**, line-based ASCII protocol parsing, bounded output buffering with backpressure handling, and a dedicated multi-threaded benchmarking suite.

## Table of Contents
1. [Architecture & Design](#architecture--design)
   - [Concurrency Engines](#concurrency-engines)
   - [Storage Engine](#storage-engine)
   - [Robustness & Lifecycle](#robustness--lifecycle)
2. [Protocol Specification](#protocol-specification)
   - [Commands & Responses](#commands--responses)
   - [Wire Protocol Examples](#wire-protocol-examples)
3. [Command-Line Interface](#command-line-interface)
4. [Building, Running & Testing](#building-running--testing)
   - [Prerequisites & Strict Compilation](#prerequisites--strict-compilation)
   - [Build Targets](#build-targets)
   - [Running the Server](#running-the-server)
   - [Automated Test Suites](#automated-test-suites)
   - [Sanitizers & Dynamic Analysis](#sanitizers--dynamic-analysis)
5. [Benchmarking & Performance](#benchmarking--performance)
   - [Benchmark Client](#benchmark-client)
   - [Automated Comparison Suite](#automated-comparison-suite)
   - [Persistent Results](#persistent-results)
   - [Illustrative Results vs. Empirical Data](#illustrative-results-vs-empirical-data)
6. [Design Trade-Offs & Limitations](#design-trade-offs--limitations)

## Architecture & Design

### Concurrency Engines

MiniServe implements two swappable network architectures configured via `--mode`:

```
+-----------------------------------------------------------------------------------+
|                                 MiniServe Server                                  |
+-----------------------------------------------------------------------------------+
       |                                                           |
       v                                                           v
+-----------------------------------+       +---------------------------------------+
|  Thread-Pool Mode (--mode threadpool)  |       |     Linux Epoll Mode (--mode epoll)   |
|                                   |       |                                       |
|  [Accept Thread]                  |       |  [Single-Threaded Event Loop]         |
|         |                         |       |         |                             |
|  [Bounded FIFO Task Queue]        |       |  [epoll_wait() Event Multiplexer]     |
|         |                         |       |         |                             |
|  [Worker Threads (1..64)]         |       |  [Non-blocking Read/Write Buffers]    |
|  - Blocking socket I/O            |       |  - Edge/Level event handling          |
|  - Independent request dispatch   |       |  - 64 KB client output buffer cap     |
+-----------------------------------+       +---------------------------------------+
       |                                                           |
       +----------------------------+------------------------------+
                                    |
                                    v
       +-----------------------------------------------------------+
       |           Thread-Safe Storage Engine (Hashtable)          |
       |  - FNV-1a Hashing with Dynamic Rehashing (Load Factor > 0.75)|
       |  - Fine-grained pthread_rwlock_t read/write synchronization|
       |  - Bounded Capacity with O(1) LRU Eviction Doubly-Linked List |
       |  - Millisecond-Precision TTL Expiration (Monotonic Clock)   |
       +-----------------------------------------------------------+
```

1. **Thread-Pool Mode (`--mode threadpool`)**:
   - A dedicated acceptor thread accepts inbound TCP connections and enqueues client file descriptors into a thread-safe, bounded FIFO queue (`queue_t`).
   - A configurable pool of worker threads (`-t, --threads`, default: 4) populates the queue using condition-variable synchronization (`pthread_cond_t`).
   - Worker threads service connections until the client closes or is terminated, supporting fragmented TCP streams, pipelined batches, and idle timeouts.

2. **Linux Epoll Mode (`--mode epoll`)**:
   - A single-threaded asynchronous event loop driven by `epoll_wait()`.
   - All listening and accepted client descriptors are configured for non-blocking operation (`O_NONBLOCK`).
   - Per-connection state structures (`epoll_conn_t`) maintain inbound read buffers and outbound write buffers (`64 KB` capacity).
   - Incomplete writes register for `EPOLLOUT` notifications; completed flushes deregister `EPOLLOUT` to prevent CPU spinning.
   - Signal handling (`SIGINT`, `SIGTERM`) uses the self-pipe trick (`pipe2(O_NONBLOCK | O_CLOEXEC)`) registered with epoll, waking the event loop cleanly for zero-leak teardown.

### Storage Engine

The storage engine (`src/hashtable.c`) is an in-memory key-value store optimized for high-throughput concurrent access:

- **Collision Resolution & Sizing**: Separate chaining with dynamic load-factor management. The table doubles bucket capacity when `num_entries / num_buckets > 0.75`, rehashing elements under exclusive write lock.
- **Hashing**: 64-bit Fowler–Noll–Vo (FNV-1a) hash function providing uniform distribution across bucket arrays.
- **Read-Write Locking**: Uses `pthread_rwlock_t` to allow arbitrary concurrent readers (`GET`, `EXISTS`, `TTL`, `COUNT`) while isolating mutating operations (`SET`, `SETEX`, `DEL`, rehashing) under exclusive write lock.
- **LRU Cache Eviction**:
  - Implements an $O(1)$ doubly-linked list tracking entry recency (`lru_head` = Most Recently Used, `lru_tail` = Least Recently Used).
  - Configurable maximum key capacity (`-K, --max-keys <num>`). When capacity is exceeded during insertion, the tail victim is atomically decoupled and freed.
  - **Deadlock-Free Lock Hierarchy**: To permit readers (`GET`, `EXISTS`) to update LRU position without acquiring exclusive table write locks, LRU list manipulation is synchronized with a dedicated `pthread_mutex_t lru_lock`. Lock acquisition order is strictly:
    $$\text{table rwlock} \longrightarrow \text{lru\_lock}$$
- **TTL Expiration**:
  - Millisecond-resolution deadlines computed via POSIX monotonic clock (`clock_gettime(CLOCK_MONOTONIC)`).
  - Passive (lazy) expiration evaluation on lookup, existence check, and key counting.
  - Deterministic testing hook (`hashtable_set_test_time_ms`) allows sub-millisecond precision validation of expiration boundaries without wall-clock sleep delays.

### Robustness & Lifecycle

- **Non-Blocking Descriptor Validation**: Return values of `fcntl(F_GETFL)` and `fcntl(F_SETFL)` are verified on both listening and accepted sockets. Startup fails cleanly if listening socket flags cannot be set, and unconfigurable client sockets are closed immediately without registering with epoll.
- **Output-Buffer Overflow Backpressure**: If a client socket kernel buffer fills up and pending responses exceed the 64 KB application buffer threshold, the connection is terminated safely to prevent memory exhaustion, closing the socket exactly once.
- **Idle Timeout & Slowloris Defense**: Configurable inactivity timeout (`-T, --idle-timeout`, default: 30 seconds). Slow or stalled connections are automatically detected and evicted.
- **Lock-Free Atomic Counters**: Server connection metrics (`active_connections`, `total_connections`, `running`) utilize C11 `<stdatomic.h>` primitives with compile-time lock-free assertions (`atomic_is_lock_free`).

## Protocol Specification

MiniServe uses a human-readable, line-delimited ASCII protocol. Requests end with `\r\n` (bare `\n` is also tolerated).

### Constraints
- **Maximum Key Length**: 64 bytes
- **Maximum Value Length**: 512 bytes
- **Maximum Request Line**: 1024 bytes (lines exceeding this length receive `ERR OVERSIZED` and are dropped)

### Commands & Responses

| Command | Syntax | Success Response | Error Response | Description |
| :--- | :--- | :--- | :--- | :--- |
| **SET** | `SET <key> <val>` | `OK\r\n` | `ERR ...` | Store key-value pair (overwrites existing, clears TTL). |
| **SETEX** | `SETEX <key> <ttl> <val>` | `OK\r\n` | `ERR ...` | Store key-value pair with TTL expiration in seconds ($1 \le \text{ttl} \le 2^{31}-1$). |
| **GET** | `GET <key>` | `VALUE <val>\r\n` | `NOT_FOUND\r\n` | Retrieve value for key. Returns `NOT_FOUND` if missing or expired. |
| **DEL** | `DEL <key>` | `OK\r\n` | `NOT_FOUND\r\n` | Remove key from store. Returns `NOT_FOUND` if missing. |
| **EXISTS**| `EXISTS <key>` | `1\r\n` | `0\r\n` | Check key existence (returns `0` if missing or expired). |
| **TTL** | `TTL <key>` | `TTL <sec>\r\n` | `-1\r\n` or `NOT_FOUND\r\n` | Get remaining TTL in seconds. Returns `-1` if persistent, `NOT_FOUND` if missing/expired. |
| **COUNT**| `COUNT` | `COUNT <N>\r\n` | — | Return total active (non-expired) key count. |
| **QUIT** | `QUIT` | `BYE\r\n` | — | Gracefully close client connection. |

### Wire Protocol Examples

```text
# Basic CRUD
C: SET user:101 Alice\r\n
S: OK\r\n
C: GET user:101\r\n
S: VALUE Alice\r\n
C: EXISTS user:101\r\n
S: 1\r\n
C: DEL user:101\r\n
S: OK\r\n
C: GET user:101\r\n
S: NOT_FOUND\r\n

# TTL Expiration (SETEX and TTL)
C: SETEX token:abc 60 secret_data\r\n
S: OK\r\n
C: TTL token:abc\r\n
S: TTL 60\r\n
C: SET config:mode production\r\n
S: OK\r\n
C: TTL config:mode\r\n
S: -1\r\n

# Termination
C: QUIT\r\n
S: BYE\r\n
```

## Command-Line Interface

```text
Usage: bin/miniserve [OPTIONS]

Options:
  -p, --port <port>             TCP port to bind (default: 8080, range: 1-65535)
  -t, --threads <num>           Number of worker threads (default: 4, range: 1-64)
  -m, --mode <mode>             Concurrency mode: 'threadpool' or 'epoll' (default: threadpool)
  -l, --log-file <file>         Log file path (default: stderr)
  -c, --max-connections <num>   Maximum connection queue capacity (default: 1024)
  -T, --idle-timeout <sec>      Client inactivity timeout in seconds (default: 30, range: 1-3600)
  -K, --max-keys <num>          Maximum key capacity before LRU eviction (default: 0 = unbounded)
  -h, --help                    Display this help message and exit
```

## Building, Running & Testing

### Prerequisites & Strict Compilation
- **Compiler**: GCC or Clang with C11 support (`-std=c11`).
- **Platform**: Linux / POSIX environment (epoll requires Linux kernel $\ge 2.6$).
- **Compilation Flags**: Builds use `-std=c11 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_GNU_SOURCE -O2`.

### Build Targets

```bash
# Build the production server executable (bin/miniserve)
make

# Build both production server and benchmark client
make all
# or
make release bench

# Build all test suites
make test-build

# Clean all binaries and intermediate build artifacts
make clean
```

### Running the Server

```bash
# Run in thread-pool mode (default, 4 threads, port 8080)
./bin/miniserve -p 8080

# Run in epoll mode with LRU cache capacity of 100,000 keys
./bin/miniserve -p 8080 -m epoll -K 100000

# Run with custom worker threads, connection queue, and file logging
./bin/miniserve -p 9000 -m threadpool -t 8 -c 2048 -l server.log
```

### Automated Test Suites

Run the complete regression test harness across all phases:

```bash
make test
```

This compiles and runs:
- `bin/test_phase1`: CLI parsing, port validation, logger bounds checking, multi-threaded logger synchronization.
- `bin/test_hashtable`: CRUD operations, rehashing at scale, boundary conditions, concurrent readers/writers.
- `bin/test_protocol`: Request parsing, oversized inputs, malformed syntax, control character filtering, trailing line terminators.
- `bin/test_threadpool`: FIFO queue ordering, try-push, producer-consumer synchronization, shutdown unblocking, worker dispatch.
- `bin/test_server`: End-to-end TCP integration, fragmented reads, pipelined commands, oversized buffers, disconnect storms, graceful teardown.
- `bin/test_robustness`: Self-pipe unblocking on `SIGINT`, idle client eviction, Slowloris mitigation, burst reconnects.
- `bin/test_epoll`: Non-blocking epoll event loop, partial-write buffering, 64 KB output overflow backpressure, simultaneous read/disconnect events.
- `bin/test_phase8`: Deterministic TTL expiration (monotonic mock time), bounded LRU eviction order, dynamic capacity adjustments, `SETEX`/`TTL` protocol validation, multi-threaded concurrent LRU safety.

### Sanitizers & Dynamic Analysis

```bash
# Build and run tests with ThreadSanitizer (TSan)
make tsan

# Run memory leak and access analysis with Valgrind
make valgrind
```

## Benchmarking & Performance

### Benchmark Client (`bench/bench_client.c`)

MiniServe includes a dedicated multi-threaded C11 benchmark harness:
- **Lockless Latency Recording**: Each client thread records latency timestamps into a pre-allocated array without mutex contention.
- **Microsecond Precision**: Measures elapsed duration using `clock_gettime(CLOCK_MONOTONIC)`.
- **Percentile Distributions**: Calculates min, mean, p50 (median), p75, p90, p95, p99, p99.9, and max latencies.
- **Strict Accounting**: Validates every server response; any connection drop or error surfaces prominently and triggers non-zero exit status.

```bash
# Build benchmark tool
make bench

# Run benchmark: 50 client threads, 2000 requests each, 80% GET / 20% SET
./bin/bench_client --port 8080 -c 50 -n 2000 --ratio 80
```

### Automated Comparison Suite (`bench/run_comparison.sh`)

Automates side-by-side benchmarking of `threadpool` (4 threads) vs. `epoll` across concurrency levels $C \in \{1, 10, 50, 100\}$:

```bash
chmod +x bench/run_comparison.sh
./bench/run_comparison.sh
```

Custom port and request volumes can be specified via environment variables:
```bash
PORT=18381 REQUESTS_PER_CLIENT=1000 ./bench/run_comparison.sh
```

### Persistent Results

Every execution of `bench/run_comparison.sh` automatically records the run configuration, metadata, and aligned results table to:
```text
bench/results/benchmark_<YYYYMMDD_HHMMSS>.txt
```
Previous runs are preserved; files are never overwritten.

### Illustrative Results vs. Empirical Data

> [!IMPORTANT]
> **Illustrative Sample Numbers**: The table below represents an **illustrative layout format** to demonstrate metric presentation and column structure. It is **not** synthetic or claimed hardware performance. To inspect genuine measured benchmarks, execute `bench/run_comparison.sh` on your Linux environment and view the logs generated in `bench/results/`.

```text
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
```

## Design Trade-Offs & Limitations

1. **In-Memory Volatility**:
   - MiniServe is an entirely in-memory data store. There is no append-only file (AOF) or snapshot persistence (RDB). Server shutdown or crash results in complete data loss.
2. **Read-Write Lock Granularity**:
   - Storage uses a single table-wide `pthread_rwlock_t`. While concurrent readers scale efficiently across CPU cores, write operations (`SET`, `DEL`, rehashing) acquire exclusive locks, blocking readers during writes. A sharded/partitioned table or lock-free skip list would provide higher write scalability.
3. **LRU Lock Contention Under Heavy Read Workloads**:
   - To update LRU recency on `GET` and `EXISTS` without upgrading the table lock to a write lock, operations acquire a dedicated `lru_lock` mutex. Under extremely high read concurrency, this mutex can become a point of contention.
4. **Single-Threaded Epoll Loop**:
   - The epoll engine runs on a single OS thread. While this eliminates thread synchronization and context switching overhead, throughput is bound by a single CPU core. A multi-reactor model with `SO_REUSEPORT` across worker threads could scale epoll across multiple cores.
5. **64 KB Output Buffer Cap**:
   - The output buffering mechanism enforces a strict 64 KB limit per client to prevent memory exhaustion from slow readers. Clients that fall behind during heavy pipelined queries are actively disconnected.
