#!/usr/bin/env python3
import socket
import struct
import time
import threading
import io
import sys

# --- PROTOCOL IMPLEMENTATION ---

class DBClient:
    def __init__(self, host='127.0.0.1', port=1234):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((host, port))
        # Use TCP_NODELAY to disable Nagle's algorithm for accurate latency benchmarks
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def close(self):
        self.sock.close()

    def _recvall(self, n):
        data = bytearray()
        while len(data) < n:
            packet = self.sock.recv(n - len(data))
            if not packet:
                raise ConnectionError("Socket closed unexpectedly")
            data.extend(packet)
        return data

    def pack_request(self, *args):
        payload = struct.pack('<I', len(args))
        for a in args:
            a_bytes = a.encode('utf-8') if isinstance(a, str) else str(a).encode('utf-8')
            payload += struct.pack('<I', len(a_bytes)) + a_bytes
        return struct.pack('<I', len(payload)) + payload

    def send_request(self, *args):
        self.sock.sendall(self.pack_request(*args))

    def read_response(self):
        len_buf = self._recvall(4)
        msg_len = struct.unpack('<I', len_buf)[0]
        msg_data = self._recvall(msg_len)
        return self._parse_response(io.BytesIO(msg_data))

    def _parse_response(self, stream):
        tag_buf = stream.read(1)
        if not tag_buf:
            return None
        tag = tag_buf[0]

        if tag == 0:   # Nil
            return None
        elif tag == 1: # Err
            code = struct.unpack('<I', stream.read(4))[0]
            msg_len = struct.unpack('<I', stream.read(4))[0]
            msg = stream.read(msg_len).decode('utf-8')
            raise RuntimeError(f"ERR {code}: {msg}")
        elif tag == 2: # Str
            msg_len = struct.unpack('<I', stream.read(4))[0]
            return stream.read(msg_len).decode('utf-8')
        elif tag == 3: # Int
            return struct.unpack('<q', stream.read(8))[0]
        elif tag == 4: # Dbl
            return struct.unpack('<d', stream.read(8))[0]
        elif tag == 5: # Arr
            n = struct.unpack('<I', stream.read(4))[0]
            return [self._parse_response(stream) for _ in range(n)]
        else:
            raise ValueError(f"Unknown response tag: {tag}")

    def request(self, *args):
        self.send_request(*args)
        return self.read_response()


# --- INTEGRATION TESTS ---

def assert_eq(expected, actual, msg):
    if expected != actual:
        print(f"  [FAIL] {msg}\n    Expected: {expected}\n    Actual:   {actual}")
        sys.exit(1)
    print(f"  [ OK ] {msg}")

def run_tests():
    print("--- Running Integration Tests ---")
    c = DBClient()
    
    # Clean up database state from previous runs
    existing_keys = c.request("keys")
    if existing_keys:
        for k in existing_keys:
            c.request("del", k)

    # Strings
    assert_eq(None, c.request("set", "foo", "bar"), "SET returns nil")
    assert_eq("bar", c.request("get", "foo"), "GET returns value")
    assert_eq(None, c.request("get", "nosuchkey"), "GET on missing key")
    assert_eq(1, c.request("del", "foo"), "DEL existing key")
    assert_eq(0, c.request("del", "foo"), "DEL already-gone key")
    
    # Lists
    c.request("rpush", "mylist", "a", "b", "c")
    assert_eq(3, c.request("llen", "mylist"), "LLEN after rpush")
    c.request("lpush", "mylist", "z")
    assert_eq("z", c.request("lpop", "mylist"), "LPOP returns head")
    assert_eq("c", c.request("rpop", "mylist"), "RPOP returns tail")
    
    try:
        c.request("get", "mylist")
        print("  [FAIL] GET on a list should throw WRONGTYPE")
        sys.exit(1)
    except RuntimeError as e:
        assert_eq(True, "WRONGTYPE" in str(e), "GET on list is WRONGTYPE")

    # Hashes
    c.request("hset", "myhash", "f1", "v1", "f2", "v2")
    assert_eq("v1", c.request("hget", "myhash", "f1"), "HGET existing field")
    assert_eq(2, c.request("hlen", "myhash"), "HLEN")
    assert_eq(1, c.request("hdel", "myhash", "f1"), "HDEL existing field")
    
    # Sets
    c.request("sadd", "myset", "a", "b", "c")
    assert_eq(3, c.request("scard", "myset"), "SCARD")
    assert_eq(1, c.request("sismember", "myset", "a"), "SISMEMBER present")
    assert_eq(0, c.request("sismember", "myset", "zzz"), "SISMEMBER absent")
    
    # ZSets
    c.request("zadd", "lb", "100", "alice")
    c.request("zadd", "lb", "90", "bob")
    c.request("zadd", "lb", "95", "carol")
    assert_eq(90.0, c.request("zscore", "lb", "bob"), "ZSCORE existing")
    assert_eq(1, c.request("zrem", "lb", "bob"), "ZREM existing member")
    
    # TTL
    c.request("set", "temp", "v")
    c.request("pexpire", "temp", "100")
    pttl = c.request("pttl", "temp")
    assert_eq(True, 0 < pttl <= 100, "PTTL is positive after SET")
    time.sleep(0.15)
    assert_eq(None, c.request("get", "temp"), "GET after TTL expiry")

    c.close()
    print("All tests passed!\n")


# --- BENCHMARKS ---

def bench_ping_pong(count):
    c = DBClient()
    start = time.perf_counter()
    for i in range(count):
        c.request("set", "bench_key", str(i))
    duration = time.perf_counter() - start
    c.close()
    
    ops = count / duration
    lat = (duration / count) * 1000
    print(f"Sequential SET (Ping-Pong): {ops:,.0f} ops/sec (Avg Latency: {lat:.3f} ms)")

def bench_pipelined(count, batch_size=1000):
    c = DBClient()
    start = time.perf_counter()
    
    for i in range(0, count, batch_size):
        # Pack and send batch
        batch = bytearray()
        for j in range(batch_size):
            batch.extend(c.pack_request("get", "bench_key"))
        c.sock.sendall(batch)
        
        # Read batch
        for j in range(batch_size):
            c.read_response()

    duration = time.perf_counter() - start
    c.close()
    print(f"Pipelined GET (Batch {batch_size}): {count / duration:,.0f} ops/sec")

def bench_concurrent(threads, ops_per_thread):
    start = time.perf_counter()
    
    def worker(tid):
        c = DBClient()
        for i in range(ops_per_thread):
            c.request("sadd", "concurrent_set", f"user_{tid}_{i}")
        c.close()

    pool = []
    for i in range(threads):
        t = threading.Thread(target=worker, args=(i,))
        t.start()
        pool.append(t)
    
    for t in pool:
        t.join()

    duration = time.perf_counter() - start
    total_ops = threads * ops_per_thread
    print(f"Concurrent SADD ({threads} threads): {total_ops / duration:,.0f} ops/sec")

def run_benchmarks():
    print("--- Running Benchmarks ---")
    bench_ping_pong(10_000)
    bench_pipelined(100_000, batch_size=2000)
    bench_concurrent(threads=20, ops_per_thread=2_000)
    print("")

if __name__ == "__main__":
    try:
        run_tests()
        run_benchmarks()
    except ConnectionError:
        print("Failed to connect. Is the database server running on 127.0.0.1:1234?")
        sys.exit(1)