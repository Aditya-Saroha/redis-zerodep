# Zero-Dep Datastore

Zero-Dep Datastore is an in-memory key-value database built using C++17 and POSIX primitives for Track D. It implements Strings, Lists, Hashes, Sets, Sorted Sets, and TTL expirations with zero third-party dependencies, utilizing direct Linux syscalls and custom data structures in place of external libraries like `libuv` or `Boost.Asio`.

## Benchmarks

A custom multi-threaded Python client (`tests/bench_and_test.py`, using only the standard `socket` and `threading` libraries) was used to measure throughput and concurrency over raw TCP sockets. The following results were recorded in a virtualized WSL2 environment:

| Workload | Throughput | Average Latency |
| :--- | :--- | :--- |
| **Pipelined GET (Batch 2000)** | **86,719 ops/sec** | — |
| **Concurrent SADD (20 threads)** | **1,614 ops/sec** | — |
| **Sequential SET (Ping-Pong)** | **1,128 ops/sec** | **0.887 ms** |

* **Pipelined GET:** Measures `epoll` and data-structure performance by batching 2,000 commands per TCP packet, removing network round-trip bottlenecks.
* **Concurrent SADD:** Demonstrates event loop multiplexing across 20 simultaneous threads. The single-threaded network I/O model handles concurrent connections safely without requiring keyspace mutexes.
* **Sequential SET:** Measures base latency. The ~0.88ms average latency primarily reflects OS context switching and virtual network stack overhead in the test environment.

## Build & Make Targets

The `Makefile` covers compilation, testing, and reproducible build proofs:

| Command | Description |
| :--- | :--- |
| `make all` | Compiles the server (`build/redis-server`) and client (`build/redis-client`). |
| `make clean` | Wipes the `build/` directory and temporary artifacts. |
| `make test-unit` | Builds and executes the unit test suite for the skiplist, hashtable, and zset. |
| `make test-integration` | Builds binaries and executes the multi-client integration test. |
| `make test` | Runs both unit and integration test suites sequentially. |
| `make reproduce-proof` | Compiles the binary twice, generates SHA-256 hashes, and verifies byte-identity. |

**Quick Start:**
```text
make all
./build/redis-server
```

## Datastore Architecture

Core operations and event loops are implemented directly in the `src/` directory:

* **Network Layer (`server.cpp`):** An edge-triggered `epoll` event loop with non-blocking sockets (`O_NONBLOCK`). Direct pointer arithmetic over raw byte buffers is used to decode the custom length-prefixed binary protocol.
* **Keyspace (`hmap.hpp`):** The top-level keyspace uses a custom open-chaining hash table with incremental rehashing. Buckets are migrated during read/write operations to distribute the rehashing cost.
* **Sorted Sets (`zset.hpp` / `skiplist.hpp`):** Ranked sets use a randomized Skiplist paired with a hash map, providing $O(\log n)$ performance for rank-based queries.
* **TTL Tracking:** A custom binary min-heap with intrusive back-references tracks expirations, allowing $O(\log n)$ updates when a key's TTL is modified.

## Concurrency Model

The datastore operates on a single-writer event loop with offloaded background tasks:

* **Thread-Safe State:** All network I/O, protocol parsing, and data modifications occur on a single thread.
* **Async Deletion (`common.hpp`):** Freeing massively populated containers synchronously blocks the event loop. The server utilizes a background thread pool (built on `pthread` and `pthread_cond_wait`) to handle asynchronous deallocation for containers exceeding 1,000 elements.
* **Async Persistence (`fork()`):** Point-in-time saves and AOF rewrites are offloaded to child processes using copy-on-write semantics.

## Persistence & Durability

The database supports both Point-in-Time Snapshots (RDB) and an Append-Only File (AOF), routed to a dynamically resolved `data/` directory. 

* **Crash Safety:** RDB snapshots and AOF rewrites write to a `.tmp` file first, followed by an atomic POSIX `rename()`.
* **Append-Only File (AOF):** Write commands are appended to `data/appendonly.aof`. An `fsync()` is issued once per second. When the AOF doubles its original size, the server triggers a background rewrite.
* **Copy-on-Write Background Persistence (`BGSAVE` / `BGREWRITEAOF`):** Both persistence paths use `fork()`. The child process walks a private, point-in-time view of the keyspace while the parent continues serving client traffic on the original pages. 
* **AOF Rewrite Buffering:** During a `BGREWRITEAOF`, the parent appends incoming writes to both the active AOF and an in-memory buffer. When the child exits, the parent appends this buffer to the child's temporary file and renames it, merging the data without interrupting durability.
* **Non-blocking reaping:** A `waitpid(..., WNOHANG)` check runs on every event-loop tick to collect finished background children without blocking. At most one background process runs at a time.

## Memory Management & Eviction

The server supports a configurable memory ceiling and pluggable eviction policies:

* **`CONFIG SET maxmemory <bytes>`:** Sets a soft ceiling, checked against the process's resident set size (`/proc/self/statm`). `0` disables enforcement (default).
* **`CONFIG SET maxmemory-policy <policy>`:** Selects the eviction strategy (`noeviction`, `allkeys-lru`, `allkeys-lfu`, `volatile-lru`, `volatile-lfu`, `volatile-ttl`). The `volatile-*` policies only target keys with an active TTL.
* **Approximate LRU/LFU:** Each key carries lightweight metadata (a `last_access_ms` timestamp and an 8-bit logarithmic LFU counter). During eviction, the server draws a small random sample of candidate keys via single-pass reservoir sampling and evicts the most eligible key in the sample based on the active policy.
* Eviction checks occur after write commands and during event-loop timer ticks, with a bounded number of evictions per check to maintain responsiveness.

## Track D Alignment & Dependency Proof

This submission targets **Track D (Data & Storage)** and passes the provided test suites. See **`STDLIB.md`** for the component substitution log documenting standard library replacements.

**Dependency Verification:**
```text
ldd build/redis-server > deps-proof.txt
cat deps-proof.txt
```
*(Expected output contains only system libraries like `linux-vdso.so`, `libpthread.so`, `libm.so`, `libstdc++.so`, `libgcc_s.so`, and `libc.so`.)*

## Bonus Claims

### +5 Hard: Reproducible Build
By enforcing `-frandom-seed` and `-ffile-prefix-map` in the Makefile (`make reproduce-proof`), the binary compiles deterministically. 

**Proof of determinism:**
```text
--- Build Run 1 ---
b17ab91fa99b1857e5c8db228d831a8b843c3e1500612c5e37e378e8538a0e25  build/redis-server

--- Build Run 2 ---
b17ab91fa99b1857e5c8db228d831a8b843c3e1500612c5e37e378e8538a0e25  build/redis-server

--- Verifying ---
SUCCESS: Binaries are byte-identical!
```

### +3 Medium: Package Killer
Avoided standard industry async I/O libraries like **`Boost.Asio`** and **`libuv`**. Network concurrency is handled entirely by a custom edge-triggered `epoll` event loop over non-blocking sockets. Details are provided in `STDLIB.md`.