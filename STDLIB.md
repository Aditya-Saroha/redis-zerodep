# STDLIB.md

This project is a C++/POSIX implementation for the Zero Dependency
hackathon. It has **zero third-party runtime dependencies** and vendors no
external source code.

The runtime is built only from:

- The C++ standard library shipped with the compiler
- The C standard library and libc system calls
- POSIX networking, polling, time, and threading APIs

The compiler, linker, GNU C++ language extensions, and WSL/Linux toolchain are
build requirements, not runtime dependencies.

## Dependency proof

The repository contains no package manager manifest, dependency lockfile, or
third-party library. The complete build is performed directly from the source
files:

```sh
g++ -std=gnu++14 -O2 -Wall -Wextra -pthread \
	src/server.cpp src/avl.cpp src/hashtable.cpp src/heap.cpp \
	src/thread_pool.cpp src/zset.cpp \
	-o build/server

g++ -std=gnu++14 -O2 -Wall -Wextra -pthread \
	src/client.cpp \
	-o build/client
```

The only linked libraries are the compiler's standard runtime and the POSIX
threading support requested by `-pthread`.

## Standard-library substitution log

| Usual dependency | Standard-library or system replacement | Why |
| --- | --- | --- |
| Redis client library | POSIX `socket`, `connect`, `read`, and `write` | The client speaks the private binary protocol directly |
| Redis server/framework | POSIX sockets plus `poll` | The server owns accept, read, write, and connection lifecycle handling |
| Event-loop library | POSIX `poll` | One non-blocking event loop handles listening and client sockets |
| Networking framework | `<sys/socket.h>`, `<arpa/inet.h>`, `<netinet/ip.h>` | TCP setup and loopback address handling |
| Buffer/ByteBuffer package | `std::vector<uint8_t>` | Incoming and outgoing byte buffers grow without a helper package |
| Serialization library | `memcpy`, fixed-width integers, and explicit tagged values | Requests and responses use a small documented binary format |
| Command-line parser | `argc`, `argv`, and `std::vector<std::string>` | The client needs only positional command arguments |
| Hash-map package | Hand-written intrusive hash table in `hashtable.cpp` | Top-level keys and sorted-set members need indexed lookup |
| Ordered-map/tree package | Hand-written AVL tree in `avl.cpp` | Sorted-set ordering and rank offsets are implemented directly |
| Sorted-set/database package | `zset.cpp` using the local AVL tree and hash table | Provides score ordering and member lookup without an external store |
| Priority queue/timer package | Hand-written binary heap in `heap.cpp` | Key expiration is scheduled by expiration timestamp |
| Timer/event scheduler | POSIX `clock_gettime` and the existing `poll` timeout | Drives TTL expiration, idle connection cleanup, and stats |
| Thread-pool package | POSIX `pthread_create`, mutexes, condition variables, and a local queue | Large sorted-set deletion runs away from the event loop |
| Logging framework | C stdio `fprintf` and `printf` | Diagnostics and client output require no logging dependency |
| Memory monitor | Linux `/proc/self/statm` plus `fscanf` | `stats` reports resident memory when available |
| Test framework | No runtime test framework | The shipped server and client have no test dependency |

## What we did not use

- No package manager or package registry
- No Redis server or Redis protocol implementation
- No Boost, hiredis, Asio, libevent, or other networking library
- No database, persistence, serialization, or logging library
- No vendored third-party source

## Platform note

The implementation targets Linux/POSIX. On Windows, build and run it inside
WSL. The `/proc/self/statm` metric is Linux-specific; if it is unavailable,
the server reports `memory_kb` as zero rather than requiring another library.
