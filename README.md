# Zero-Dep Datastore

**Zero-Dep Datastore** is an in-memory key-value database built entirely from scratch using C++17 and POSIX primitives for Track D. It implements the core architecture of Redis—including Strings, Lists, Hashes, Sets, Sorted Sets, and TTL expirations—without a single third-party dependency. 

## Build & Make Targets

The project includes an exhaustive `Makefile` covering compilation, unit tests, integration suites, and reproducible build proofs:

| Command | Description |
| :--- | :--- |
| `make all` | Compiles the server (`build/redis-server`) and client (`build/redis-client`). |
| `make clean` | Wipes the `build/` directory and temporary artifacts. |
| `make test-unit` | Builds and executes the unit test suite for the skiplist, hashtable, and zset. |
| `make test-integration` | Builds binaries and executes the multi-client bash integration test. |
| `make test` | Runs both unit and integration test suites sequentially. |
| `make reproduce-proof` | Compiles the binary twice, generates SHA-256 hashes, and verifies byte-identity. |

**Quick Start:**
```bash
make all
./build/redis-server
```

---

## Datastore Architecture

To avoid massive third-party packages, core data structures and event loops were hand-rolled across a clean modular layout (`src/`):

* **Network Layer (`server.cpp`):** An edge-triggered `epoll` event loop with non-blocking sockets (`O_NONBLOCK`). Direct pointer arithmetic over raw byte buffers decodes the custom length-prefixed binary protocol.
* **Keyspace (`hmap.hpp`):** The top-level keyspace uses a custom open-chaining hash table with incremental rehashing, migrating buckets during read/write operations to guarantee flat latency.
* **Sorted Sets (`zset.hpp` / `skiplist.hpp`):** Ranked sets use a custom randomized `Skiplist` paired with a hash map, providing $O(\log n)$ performance for rank-based queries.
* **TTL Tracking:** A custom binary min-heap with intrusive back-references tracks expirations, allowing $O(\log n)$ updates when a key's TTL is modified.

---

## Concurrency Model

The datastore operates on a **Single-Writer Event Loop** augmented by **Background Workers**.

* **Thread-Safe State:** All network I/O, protocol parsing, and data modifications occur on a single thread, strictly eliminating race conditions and requiring zero mutexes for the keyspace.
* **Async Deletion (`common.hpp`):** When deleting a massively populated container, freeing it synchronously would block the event loop. The server utilizes a background thread pool (built on raw `pthread` and `pthread_cond_wait`) to handle asynchronous deallocation for containers exceeding 1,000 elements.

---

## Persistence & Durability Guarantees

The database supports both Point-in-Time Snapshots (RDB) and an Append-Only File (AOF), routed to a dynamically resolved `data/` directory.

### Append-Only File (AOF)
Every successful write command appends to `data/appendonly.aof`. 
* **Durability Guarantee:** Issues an `fsync()` exactly once per second. Maximum data loss in a crash is bounded to 1 second of writes.
* **Auto-Compaction:** When the AOF size doubles its original size, the server dynamically rewrites and compacts the state.

### RDB Snapshots
Periodic, point-in-time binary snapshots save to `data/dump.rdb`.
* **Crash Safety:** Both RDB snapshots and AOF rewrites write to a `.tmp` file first, followed by an atomic POSIX `rename()`, preventing corruption if the server crashes mid-save.

---

## Track D Alignment & Dependency Proof

This submission targets **Track D (Data & Storage)**. It passes the full test suite (`make test`). See **`STDLIB.md`** for the complete 14-item component substitution log.

**To verify zero dependencies:**
```text
ldd build/redis-server > deps-proof.txt
cat deps-proof.txt
```
*(Shows only `linux-vdso.so`, `libpthread.so`, `libm.so`, `libstdc++.so`, `libgcc_s.so`, and `libc.so`.)*

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
Successfully avoided industry-default network and protocol crates. Completely replaced **`Boost.Asio` / `libuv`** (via a hand-rolled `epoll` event loop) and **`hiredis`** (via a custom length-prefixed binary protocol parser). See `STDLIB.md` for details.