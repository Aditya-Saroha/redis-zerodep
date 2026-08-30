# STDLIB.md

### The "Zero Dependency" Log

In building a single-threaded, high-concurrency, persistent key-value datastore in C++, avoiding third-party packages meant replacing standard industry libraries with raw POSIX primitives and custom data structures. 

Here are the 14 actual, defensible stdlib-for-package substitutions used to keep the build system completely free of external linkage.

**1. Async Networking Frameworks (`libuv` / `Boost.Asio`) -> `<sys/epoll.h>`**
Instead of pulling in an async networking library, the server manages thousands of concurrent client connections using a hand-rolled, edge-triggered `epoll` event loop with non-blocking sockets (`O_NONBLOCK`).

**2. High-Performance Maps (`absl::flat_hash_map`) -> Custom Incremental `HMap`**
`std::unordered_map` triggers stop-the-world rehashes that stall a single-threaded database; the keyspace uses a custom open-chaining hash table that migrates buckets incrementally during read/write operations to guarantee flat latency.

**3. Balanced Tree Libraries (`std::map` / B-Tree crates) -> Custom Randomized `Skiplist`**
To back `ZSET` commands without importing external tree libraries, sorted sets are implemented via a custom randomized skiplist providing $O(\log n)$ ranked queries and score ranges.

**4. Storage Engines (`RocksDB`) -> Custom AOF + RDB Snapshotter**
Rather than embedding a heavy storage engine, persistence is implemented via a point-in-time binary snapshotter and an append-only file (AOF) logger using raw POSIX `fsync()` and atomic file replacement (`rename()`).

**5. Wire Protocols & Serialization (`protobuf` / `hiredis`) -> Raw Byte Pointers**
Communication between client and server bypasses serialization libraries entirely, using a custom length-prefixed binary protocol decoded via direct pointer arithmetic over raw socket buffers.

**6. Task Queues (`Intel TBB` / `std::async`) -> `<pthread.h>` + `std::deque`**
To prevent massive container deletions from blocking the single-threaded event loop, asynchronous memory deallocation is offloaded to a background worker pool built on raw POSIX threads and condition variables.

**7. Priority Queues (`boost::heap`) -> Custom Back-Referencing Min-Heap**
TTL tracking requires updating existing expiration timestamps in $O(\log n)$ time; because standard priority queues lack fast internal updates, a custom binary min-heap with intrusive back-references was implemented.

**8. Node-based Containers (`std::list`) -> Custom Intrusive `DList`**
Idle client connections are tracked using a manual, intrusive doubly-linked list (`DList`), eliminating the heap allocation overhead and cache-miss penalty of standard container nodes.

**9. Path Resolution Libraries (`boost::filesystem`) -> `readlink("/proc/self/exe")`**
To dynamically locate the `data/` storage directory relative to the binary without pulling in path-handling packages, the executable path is queried natively from Linux `procfs`.

**10. String Formatting Libraries (`fmt` / `absl::StrFormat`) -> `snprintf`**
Double-to-string serialization in the AOF rewrite engine uses standard stack-allocated `snprintf` buffers to avoid heavy stream formatting overhead.

**11. Polymorphic Containers (`std::variant` / `std::any`) -> Tagged Union (`Entry`)**
Keyspace entries support multiple data types (Strings, Lists, Hashes, Sets, ZSets) via a manual `ObjType` enum tag and explicit pointer allocation within a unified `Entry` struct, avoiding virtual table overhead.

**12. Non-Cryptographic Hashing Libraries (`xxHash`) -> Inline FNV-1a**
Key distribution across the incremental hash map is handled by an inline multiplicative hash implementation based on standard FNV-1a constants.

**13. Signal Handlers (`libevent` signals) -> `sigaction()`**
Graceful shutdown behavior (intercepting `SIGINT` and `SIGTERM` to flush logs and write safety snapshots) is wired directly into POSIX `<signal.h>`.

**14. Command Dispatchers (`CLI11` / HTTP Routers) -> Function Pointer Registry**
Incoming protocol commands are routed and validated using an $O(1)$ static function pointer registry (`k_commands`) that enforces minimum argument counts before execution.