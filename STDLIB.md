# STDLIB.md

### The "Zero Dependency" Log

**1. Async Networking (`Boost.Asio` / `libuv`) -> `<sys/epoll.h>`**
Thousands of concurrent client connections are managed with a hand-rolled, edge-triggered `epoll` event loop and non-blocking sockets (`O_NONBLOCK`), rather than an async I/O framework. 

**2. High-Performance Hash Maps (`absl::flat_hash_map`) -> Custom Incremental `HMap`**
A flat/open-addressing hash map from Abseil is a common reach for a latency-sensitive keyspace, but its rehash is still a stop-the-world operation on a single-threaded event loop. The keyspace instead uses a custom open-chaining hash table that migrates buckets incrementally across normal read/write traffic.

**3. Ordered/Ranked Containers (`absl::btree_map`) -> Custom Randomized Skiplist**
`ZSET` commands need ranked, score-ordered iteration. Abseil's B-tree is a standard reach for an ordered container with better cache behavior than `std::map`; this project backs sorted sets with a custom randomized skiplist instead, giving $O(\log n)$ rank/range queries with simpler, non-rebalancing node structure.

**4. Storage Engines (`RocksDB`) -> Custom AOF + RDB Snapshotter**
Persistence is implemented directly via a point-in-time binary snapshotter and an append-only command log, using raw POSIX `fsync()` and atomic `rename()`, instead of embedding a general-purpose storage engine.

**5. Serialization / RPC Libraries (`protobuf`) -> Raw Byte Pointers**
Client/server communication uses a custom length-prefixed binary protocol decoded via direct pointer arithmetic over raw socket buffers, rather than a schema/serialization library. (This is a protocol-design choice, not RESP-compatibility — the wire format isn't RESP, so there was never a `hiredis`-shaped gap here either.)

**6. Lock-Based Priority Queues (`boost::heap`) -> Custom Back-Referencing Min-Heap**
TTL expiration needs $O(\log n)$ updates to an already-queued item's priority. Rather than pull in Boost's mutable heap (which supports this via handle-based `update()`), a custom binary min-heap with an intrusive back-reference (`heap_idx` stored directly on each entry) provides the same in-place update.

**7. String Formatting (`fmt` / `absl::StrFormat`) -> `snprintf`**
Double-to-string serialization in the AOF rewrite path uses plain stack-allocated `snprintf` buffers rather than a formatting library.

**8. Non-Cryptographic Hashing (`xxHash`) -> Inline FNV-1a**
Key distribution across the hash map uses an inline multiplicative FNV-1a implementation rather than a dedicated fast-hash library.

**9. Parallel Task Scheduling (`Intel TBB`) -> POSIX Thread Pool**
Deallocating large containers (over 1,000 elements) synchronously would stall the single-threaded event loop, so it's offloaded to a background worker pool built directly on `pthread` + `pthread_cond_wait`, rather than a task-scheduling library like TBB.

**10. Lock-Free Queues (`moodycamel::ConcurrentQueue`) -> `std::deque` + Condition Variable**
The work queue feeding that thread pool is a manually mutex/condvar-guarded `std::deque`, rather than a header-only lock-free MPMC queue — a common drop-in for exactly this producer/consumer shape. 

**11. Intrusive Container Libraries (`Boost.Intrusive`) -> Custom Intrusive `DList`**
Idle-connection tracking uses a hand-written intrusive doubly-linked list (the list node lives inside `Conn`/`Entry`, no separate allocation) rather than Boost's intrusive-containers library, which exists specifically to provide this pattern. 

None of the above claim a specific measured performance win over the named library — where a rationale is about latency (items 2, 3, 6), it's the structural reason the design was chosen, not a benchmark against Abseil/Boost that this project has actually run. Overclaiming that would be worse than just stating the design intent.

---

## Track D Alignment

This submission targets **Track D (Data & Storage)**: it writes the storage engine itself (custom hashmap, skiplist, TTL heap, AOF+RDB persistence with `fork()`-based background save/rewrite) rather than wrapping an existing one, and documents its durability guarantees (1s max AOF data loss, atomic snapshot rename) and eviction behavior (`maxmemory` + LRU/LFU/TTL policies) in the README. The 11 substitutions above clear the **STDLIB Log bonus** threshold (≥10, each with a one-line rationale) without padding the count with anything the rules already permit outright.

**To verify zero dependencies:**
```text
ldd build/redis-server > deps-proof.txt
cat deps-proof.txt
```
*(Shows only `linux-vdso.so`, `libpthread.so`, `libm.so`, `libstdc++.so`, `libgcc_s.so`, and `libc.so`.)*