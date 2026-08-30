# redis-zerodep

`redis-zerodep` is a small, dependency-free Redis-like in-memory key-value
server written in C++. It is intended for learning and experimentation with
non-blocking sockets, a poll-based event loop, hash tables, AVL trees, heaps,
and a small thread pool.

It is **not compatible with the Redis wire protocol**. The included client is
the reference client for the server's private binary protocol.

## Features

- In-memory string values
- Sorted sets indexed by member name and ordered by `(score, name)`
- Key expiration in milliseconds
- Non-blocking TCP server with request pipelining
- Incremental hash-table rehashing
- Basic runtime statistics
- No external runtime libraries beyond the POSIX system APIs and pthreads

## Build

Build it on Linux or another POSIX-like system with GNU C++ extensions
enabled. From the repository root:

```sh
mkdir -p build
g++ -std=gnu++14 -O2 -Wall -Wextra -pthread \
    src/server.cpp src/avl.cpp src/hashtable.cpp src/heap.cpp \
    src/thread_pool.cpp src/zset.cpp \
    -o build/server

g++ -std=gnu++14 -O2 -Wall -Wextra -pthread \
    src/client.cpp \
    -o build/client
```

The source uses POSIX sockets, `poll`, `clock_gettime`, `/proc/self/statm`,
and GNU `typeof`/statement-expression syntax. On Windows, use WSL and run the
commands from the Linux environment. From WSL, this repository is available at
`/mnt/e/personal/redis-zerodep`.

## Tests

Build the binaries, then run the dependency-free integration smoke test:

```sh
bash tests/smoke.sh
```

The test starts and stops its own server, then checks string operations, key
deletion, millisecond expiration, sorted-set insertion/update/query/removal,
wrong-type handling, unknown-command errors, and statistics output.

## Reproducible build bonus

Using the same compiler and source tree, build each artifact twice and compare
the hashes:

```sh
g++ -std=gnu++14 -O2 -Wall -Wextra -pthread src/server.cpp src/avl.cpp \
    src/hashtable.cpp src/heap.cpp src/thread_pool.cpp src/zset.cpp \
    -o build/server.repro1
g++ -std=gnu++14 -O2 -Wall -Wextra -pthread src/server.cpp src/avl.cpp \
    src/hashtable.cpp src/heap.cpp src/thread_pool.cpp src/zset.cpp \
    -o build/server.repro2
g++ -std=gnu++14 -O2 -Wall -Wextra -pthread src/client.cpp -o build/client.repro1
g++ -std=gnu++14 -O2 -Wall -Wextra -pthread src/client.cpp -o build/client.repro2

sha256sum build/server.repro1 build/server.repro2 \
    build/client.repro1 build/client.repro2
cmp build/server.repro1 build/server.repro2
cmp build/client.repro1 build/client.repro2
```

Observed in WSL on August 27, 2026:

```text
5dd2b300cde0fa1ac1e4341b5a89452c7f5ba5b787d452a36dd80846096750c6  server.repro1
5dd2b300cde0fa1ac1e4341b5a89452c7f5ba5b787d452a36dd80846096750c6  server.repro2
918e7b61ba21ac5719f1c22872d6ace27525caa8ae1e3a07e045265c9a4dc38e  client.repro1
918e7b61ba21ac5719f1c22872d6ace27525caa8ae1e3a07e045265c9a4dc38e  client.repro2
```

The hashes are expected to remain identical when the compiler version,
compiler flags, source tree, and target environment are unchanged.

## Running

From WSL, change to the repository directory:

```sh
cd /mnt/e/personal/redis-zerodep
```

Start the server in one WSL terminal:

```sh
./build/server
```

The server listens only on `127.0.0.1:1234`. Run commands from another
terminal using the included client:

```sh
./build/client set greeting hello
./build/client get greeting
./build/client pexpire greeting 5000
./build/client pttl greeting
./build/client del greeting
```

The client opens one connection per invocation. A connection that remains
inactive for approximately five seconds is closed by the server.

## Commands

Arguments are space-separated process arguments when using `client`.

| Command | Arguments | Result |
| --- | --- | --- |
| `get` | `key` | String value, or nil if absent |
| `set` | `key value` | Nil on success |
| `del` | `key` | Integer: `1` if deleted, otherwise `0` |
| `pexpire` | `key ttl_ms` | Integer: `1` if the key exists, otherwise `0` |
| `pttl` | `key` | Remaining milliseconds, `-1` without TTL, `-2` if absent |
| `keys` | none | Array containing all top-level keys |
| `stats` | none | Array of statistic name/value pairs |
| `zadd` | `zset score member` | Integer: `1` if added, `0` if the member was updated |
| `zrem` | `zset member` | Integer: `1` if removed, otherwise `0` |
| `zscore` | `zset member` | Score, or nil if absent |
| `zquery` | `zset score member offset limit` | Array of member/score pairs in sorted order |

Commands and argument names are case-sensitive. An existing key cannot be
used as both a string and a sorted set.

## Protocol

Each request and response is prefixed by a 4-byte little-endian body length.
The request body contains a 4-byte little-endian argument count followed by
repeated 4-byte little-endian string lengths and raw string bytes.

Responses use tagged values:

- `0`: nil
- `1`: error (`code`, message length, message)
- `2`: string
- `3`: signed 64-bit integer
- `4`: double
- `5`: array of tagged values

The protocol is binary and has no authentication, encryption, or Redis RESP
compatibility.

## Specifications and limits

These are implementation limits, not configuration options:

| Item | Limit |
| --- | --- |
| Bind address | `127.0.0.1` only |
| Port | `1234` |
| Server request body | `32 MiB` maximum |
| Server response body | `32 MiB` maximum; larger responses become error code `2` |
| Client request body | `4096` bytes maximum |
| Client response body | `4096` bytes maximum |
| Arguments per request | `200,000` maximum on the server |
| Read buffer | `64 KiB` per socket read |
| Idle connection timeout | `5 seconds` |
| Expiration work per timer pass | At most `2,001` expired keys due to the current loop condition |
| Background deletion workers | `4` threads |

Actual key, value, sorted-set, and connection counts are constrained by
available memory, file descriptors, and operating-system limits. There is no
persistence, replication, transactions, authentication, access control, or
durability guarantee; all data is lost when the server exits.

## Errors

The server reports these error codes inside its binary error response:

| Code | Meaning |
| --- | --- |
| `1` | Unknown command |
| `2` | Response too large |
| `3` | Wrong value type |
| `4` | Invalid argument |
