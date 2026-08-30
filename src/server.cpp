#include "common.hpp"
#include "hmap.hpp"
#include "skiplist.hpp"
#include "zset.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <unordered_map>

static void msg_errno(const char *msg) { fprintf(stderr, "[errno:%d] %s\n", errno, msg); }
static void die(const char *msg) {
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

static uint64_t get_monotonic_msec() {
    struct timespec tv = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return uint64_t(tv.tv_sec) * 1000 + tv.tv_nsec / 1000 / 1000;
}

static long get_rss_kb() {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

static void fd_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) die("fcntl(F_GETFL)");
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) die("fcntl(F_SETFL)");
}

constexpr size_t k_max_msg = 32 << 20;

inline void buf_append(Buffer &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

inline void buf_consume(Buffer &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    uint32_t epoll_events = 0;
    Buffer incoming;
    Buffer outgoing;
    uint64_t last_active_ms = 0;
    DList idle_node;
};

struct ServerState {
    HMap db;
    std::vector<Conn *> fd2conn;
    DList idle_list;
    std::vector<HeapItem> heap;
    ThreadPool thread_pool;
    int epfd = -1;
    
    uint64_t start_time_ms = 0;
    uint64_t active_conns = 0;
    uint64_t total_conns = 0;
    uint64_t total_cmds = 0;
    uint64_t cmds_this_second = 0;
    uint64_t ops_per_sec = 0;
    
    uint64_t last_stats_ms = 0;
    uint64_t last_save_ms = 0;
    uint64_t ops_since_save = 0;
    
    FILE *aof_fp = nullptr;
    uint64_t aof_size = 0;
    uint64_t aof_size_at_last_rewrite = 0;
    uint64_t last_aof_fsync_ms = 0;
    bool aof_dirty_since_fsync = false;
} g_server;

static void conn_update_epoll(Conn *conn) {
    uint32_t events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (conn->want_write) events |= EPOLLOUT;
    if (events == conn->epoll_events) return;
    
    struct epoll_event ev = {};
    ev.events = events;
    ev.data.fd = conn->fd;
    if (epoll_ctl(g_server.epfd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
        msg_errno("epoll_ctl(MOD) error");
    }
    conn->epoll_events = events;
}

static int32_t handle_accept(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) msg_errno("accept() error");
        return -1;
    }
    
    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "[ACCEPT] fd=%d from %u.%u.%u.%u:%u\n",
        connfd, ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24, ntohs(client_addr.sin_port));

    fd_set_nb(connfd);
    Conn *conn = new Conn();
    conn->fd = connfd;
    dlist_init(&conn->idle_node);
    conn->want_read = true;
    conn->last_active_ms = get_monotonic_msec();
    dlist_insert_before(&g_server.idle_list, &conn->idle_node);

    if (g_server.fd2conn.size() <= static_cast<size_t>(conn->fd)) {
        g_server.fd2conn.resize(conn->fd + 1);
    }
    assert(!g_server.fd2conn[conn->fd]);
    g_server.fd2conn[conn->fd] = conn;
    g_server.total_conns++;
    g_server.active_conns++;

    struct epoll_event ev = {};
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    ev.data.fd = conn->fd;
    conn->epoll_events = ev.events;
    if (epoll_ctl(g_server.epfd, EPOLL_CTL_ADD, conn->fd, &ev) < 0) {
        msg_errno("epoll_ctl(ADD) error");
    }
    return 0;
}

static void conn_destroy(Conn *conn) {
    epoll_ctl(g_server.epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
    close(conn->fd);
    g_server.fd2conn[conn->fd] = nullptr;
    dlist_detach(&conn->idle_node);
    g_server.active_conns--;
    delete conn;
}

constexpr size_t k_max_args = 200 * 1000;

static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if (cur + 4 > end) return false;
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

static bool read_str(const uint8_t *&cur, const uint8_t *end, size_t n, std::string &out) {
    if (cur + n > end) return false;
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

static int32_t parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out) {
    const uint8_t *end = data + size;
    uint32_t nstr = 0;
    if (!read_u32(data, end, nstr)) return -1;
    if (nstr > k_max_args) return -1;

    while (out.size() < nstr) {
        uint32_t len = 0;
        if (!read_u32(data, end, len)) return -1;
        out.emplace_back();
        if (!read_str(data, end, len, out.back())) return -1;
    }
    return (data == end) ? 0 : -1;
}

enum class ErrCode : uint32_t { Unknown = 1, TooBig = 2, BadType = 3, BadArg = 4 };
enum class RespTag : uint8_t { Nil = 0, Err = 1, Str = 2, Int = 3, Dbl = 4, Arr = 5 };

static void buf_append_u8(Buffer &buf, uint8_t data) { buf.push_back(data); }
static void buf_append_u32(Buffer &buf, uint32_t data) { buf_append(buf, (const uint8_t *)&data, 4); }
static void buf_append_i64(Buffer &buf, int64_t data) { buf_append(buf, (const uint8_t *)&data, 8); }
static void buf_append_dbl(Buffer &buf, double data) { buf_append(buf, (const uint8_t *)&data, 8); }

static void out_nil(Buffer &out) { buf_append_u8(out, static_cast<uint8_t>(RespTag::Nil)); }
static void out_str(Buffer &out, const char *s, size_t size) {
    buf_append_u8(out, static_cast<uint8_t>(RespTag::Str));
    buf_append_u32(out, static_cast<uint32_t>(size));
    buf_append(out, reinterpret_cast<const uint8_t*>(s), size);
}
static void out_str(Buffer &out, const std::string &s) { out_str(out, s.data(), s.size()); }
static void out_int(Buffer &out, int64_t val) {
    buf_append_u8(out, static_cast<uint8_t>(RespTag::Int));
    buf_append_i64(out, val);
}
static void out_dbl(Buffer &out, double val) {
    buf_append_u8(out, static_cast<uint8_t>(RespTag::Dbl));
    buf_append_dbl(out, val);
}
static void out_err(Buffer &out, ErrCode code, const std::string &msg) {
    buf_append_u8(out, static_cast<uint8_t>(RespTag::Err));
    buf_append_u32(out, static_cast<uint32_t>(code));
    buf_append_u32(out, static_cast<uint32_t>(msg.size()));
    buf_append(out, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
}
static void out_arr(Buffer &out, uint32_t n) {
    buf_append_u8(out, static_cast<uint8_t>(RespTag::Arr));
    buf_append_u32(out, n);
}
static size_t out_begin_arr(Buffer &out) {
    out.push_back(static_cast<uint8_t>(RespTag::Arr));
    buf_append_u32(out, 0);
    return out.size() - 4;
}
static void out_end_arr(Buffer &out, size_t ctx, uint32_t n) {
    assert(out[ctx - 1] == static_cast<uint8_t>(RespTag::Arr));
    memcpy(&out[ctx], &n, 4);
}

enum class ObjType : uint32_t { Init = 0, Str = 1, ZSet = 2, List = 3, Hash = 4, Set = 5 };

struct Entry {
    struct HNode node;
    std::string key;
    size_t heap_idx = static_cast<size_t>(-1);
    ObjType type = ObjType::Init;

    std::string *str = nullptr;
    ZSet *zset = nullptr;
    std::deque<std::string> *list = nullptr;
    std::unordered_map<std::string, std::string> *hash = nullptr;
    std::unordered_map<std::string, char> *set = nullptr;
};

static const char *type_name(ObjType t) {
    switch (t) {
        case ObjType::Str: return "string";
        case ObjType::ZSet: return "zset";
        case ObjType::List: return "list";
        case ObjType::Hash: return "hash";
        case ObjType::Set: return "set";
        default: return "unknown";
    }
}

static Entry *entry_new(ObjType type) {
    Entry *ent = new Entry();
    ent->type = type;
    switch (type) {
        case ObjType::Str: ent->str = new std::string(); break;
        case ObjType::ZSet: ent->zset = new ZSet(); zset_init(ent->zset); break;
        case ObjType::List: ent->list = new std::deque<std::string>(); break;
        case ObjType::Hash: ent->hash = new std::unordered_map<std::string, std::string>(); break;
        case ObjType::Set: ent->set = new std::unordered_map<std::string, char>(); break;
        default: break;
    }
    return ent;
}

static void entry_set_ttl(Entry *ent, int64_t ttl_ms);

static void entry_del_sync(Entry *ent) {
    switch (ent->type) {
        case ObjType::Str: delete ent->str; break;
        case ObjType::ZSet: zset_clear(ent->zset); delete ent->zset; break;
        case ObjType::List: delete ent->list; break;
        case ObjType::Hash: delete ent->hash; break;
        case ObjType::Set: delete ent->set; break;
        default: break;
    }
    delete ent;
}

static void entry_del_func(void *arg) { entry_del_sync(static_cast<Entry *>(arg)); }

static size_t entry_size(Entry *ent) {
    switch (ent->type) {
        case ObjType::ZSet: return ent->zset->sl ? ent->zset->sl->length : 0;
        case ObjType::List: return ent->list->size();
        case ObjType::Hash: return ent->hash->size();
        case ObjType::Set: return ent->set->size();
        default: return 0;
    }
}

static void entry_del(Entry *ent) {
    entry_set_ttl(ent, -1);
    constexpr size_t k_large_container_size = 1000;
    if (entry_size(ent) > k_large_container_size) {
        g_server.thread_pool.queue_work(&entry_del_func, ent);
    } else {
        entry_del_sync(ent);
    }
}

struct LookupKey {
    struct HNode node;
    std::string key;
};

static bool entry_eq(HNode *node, HNode *key) {
    Entry *ent = container_of(node, Entry, node);
    LookupKey *keydata = container_of(key, LookupKey, node);
    return ent->key == keydata->key;
}

static Entry *lookup_entry(const std::string &key) {
    LookupKey lk;
    lk.key = key;
    lk.node.hcode = str_hash(reinterpret_cast<const uint8_t*>(lk.key.data()), lk.key.size());
    HNode *node = hm_lookup(&g_server.db, &lk.node, &entry_eq);
    return node ? container_of(node, Entry, node) : nullptr;
}

static Entry *get_or_create(const std::string &key, ObjType type, Buffer &out, bool *created) {
    Entry *ent = lookup_entry(key);
    if (ent) {
        if (ent->type != type) {
            out_err(out, ErrCode::BadType, std::string("WRONGTYPE key holds a ") + type_name(ent->type));
            return nullptr;
        }
        *created = false;
        return ent;
    }
    ent = entry_new(type);
    ent->key = key;
    ent->node.hcode = str_hash(reinterpret_cast<const uint8_t*>(key.data()), key.size());
    hm_insert(&g_server.db, &ent->node);
    *created = true;
    return ent;
}

static void do_get(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != ObjType::Str) return out_err(out, ErrCode::BadType, "WRONGTYPE not a string");
    return out_str(out, *ent->str);
}

static void do_set(const std::vector<std::string> &cmd, Buffer &out) {
    bool created;
    Entry *ent = get_or_create(cmd[1], ObjType::Str, out, &created);
    if (!ent) return;
    *ent->str = cmd[2];
    return out_nil(out);
}

static void do_del(const std::vector<std::string> &cmd, Buffer &out) {
    LookupKey key;
    key.key = cmd[1];
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t*>(key.key.data()), key.key.size());
    HNode *node = hm_delete(&g_server.db, &key.node, &entry_eq);
    if (node) entry_del(container_of(node, Entry, node));
    return out_int(out, node ? 1 : 0);
}

static void heap_delete(std::vector<HeapItem> &a, size_t pos) {
    a[pos] = a.back();
    a.pop_back();
    if (pos < a.size()) heap_update(a.data(), pos, a.size());
}

static void heap_upsert(std::vector<HeapItem> &a, size_t pos, HeapItem t) {
    if (pos < a.size()) {
        a[pos] = t;
    } else {
        pos = a.size();
        a.push_back(t);
    }
    heap_update(a.data(), pos, a.size());
}

static void entry_set_ttl(Entry *ent, int64_t ttl_ms) {
    if (ttl_ms < 0 && ent->heap_idx != static_cast<size_t>(-1)) {
        heap_delete(g_server.heap, ent->heap_idx);
        ent->heap_idx = static_cast<size_t>(-1);
    } else if (ttl_ms >= 0) {
        uint64_t expire_at = get_monotonic_msec() + static_cast<uint64_t>(ttl_ms);
        HeapItem item = {expire_at, &ent->heap_idx};
        heap_upsert(g_server.heap, ent->heap_idx, item);
    }
}

static bool str2int(const std::string &s, int64_t &out) {
    if (s.empty()) return false;
    char *endp = nullptr;
    out = strtoll(s.c_str(), &endp, 10);
    return endp == s.c_str() + s.size();
}

static bool str2dbl(const std::string &s, double &out) {
    if (s.empty()) return false;
    char *endp = nullptr;
    out = strtod(s.c_str(), &endp);
    return endp == s.c_str() + s.size() && !std::isnan(out);
}

static void do_pexpire(const std::vector<std::string> &cmd, Buffer &out) {
    int64_t ttl_ms = 0;
    if (!str2int(cmd[2], ttl_ms)) return out_err(out, ErrCode::BadArg, "expect int64");
    Entry *ent = lookup_entry(cmd[1]);
    if (ent) entry_set_ttl(ent, ttl_ms);
    return out_int(out, ent ? 1 : 0);
}

static void do_pttl(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, -2);
    if (ent->heap_idx == static_cast<size_t>(-1)) return out_int(out, -1);
    uint64_t expire_at = g_server.heap[ent->heap_idx].val;
    uint64_t now_ms = get_monotonic_msec();
    return out_int(out, expire_at > now_ms ? (expire_at - now_ms) : 0);
}

static void do_keys(const std::vector<std::string> &, Buffer &out) {
    out_arr(out, static_cast<uint32_t>(hm_size(&g_server.db)));
    auto cb_keys = [](HNode *node, void *arg) -> bool {
        out_str(*static_cast<Buffer *>(arg), container_of(node, Entry, node)->key);
        return true;
    };
    hm_foreach(&g_server.db, cb_keys, &out);
}

static void do_lpush(const std::vector<std::string> &cmd, Buffer &out) {
    bool created;
    Entry *ent = get_or_create(cmd[1], ObjType::List, out, &created);
    if (!ent) return;
    for (size_t i = 2; i < cmd.size(); i++) ent->list->push_front(cmd[i]);
    return out_int(out, ent->list->size());
}

static void do_rpush(const std::vector<std::string> &cmd, Buffer &out) {
    bool created;
    Entry *ent = get_or_create(cmd[1], ObjType::List, out, &created);
    if (!ent) return;
    for (size_t i = 2; i < cmd.size(); i++) ent->list->push_back(cmd[i]);
    return out_int(out, ent->list->size());
}

static void do_lpop(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != ObjType::List) return out_err(out, ErrCode::BadType, "WRONGTYPE not a list");
    if (ent->list->empty()) return out_nil(out);
    std::string v = ent->list->front();
    ent->list->pop_front();
    return out_str(out, v);
}

static void do_rpop(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != ObjType::List) return out_err(out, ErrCode::BadType, "WRONGTYPE not a list");
    if (ent->list->empty()) return out_nil(out);
    std::string v = ent->list->back();
    ent->list->pop_back();
    return out_str(out, v);
}

static void do_llen(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != ObjType::List) return out_err(out, ErrCode::BadType, "WRONGTYPE not a list");
    return out_int(out, ent->list->size());
}

static void do_lrange(const std::vector<std::string> &cmd, Buffer &out) {
    int64_t start = 0, stop = 0;
    if (!str2int(cmd[2], start) || !str2int(cmd[3], stop)) return out_err(out, ErrCode::BadArg, "expect int");
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_arr(out, 0);
    if (ent->type != ObjType::List) return out_err(out, ErrCode::BadType, "WRONGTYPE not a list");

    int64_t n = ent->list->size();
    if (start < 0) start = std::max<int64_t>(n + start, 0);
    if (stop < 0) stop = n + stop;
    if (stop >= n) stop = n - 1;
    if (start > stop || n == 0) return out_arr(out, 0);

    out_arr(out, static_cast<uint32_t>(stop - start + 1));
    for (int64_t i = start; i <= stop; i++) out_str(out, (*ent->list)[i]);
}

static void do_hset(const std::vector<std::string> &cmd, Buffer &out) {
    if ((cmd.size() - 2) % 2 != 0) return out_err(out, ErrCode::BadArg, "wrong number of arguments");
    bool created;
    Entry *ent = get_or_create(cmd[1], ObjType::Hash, out, &created);
    if (!ent) return;
    int64_t added = 0;
    for (size_t i = 2; i + 1 < cmd.size(); i += 2) {
        added += (*ent->hash).count(cmd[i]) == 0;
        (*ent->hash)[cmd[i]] = cmd[i + 1];
    }
    return out_int(out, added);
}

static void do_hget(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != ObjType::Hash) return out_err(out, ErrCode::BadType, "WRONGTYPE not a hash");
    auto it = ent->hash->find(cmd[2]);
    if (it == ent->hash->end()) return out_nil(out);
    return out_str(out, it->second);
}

static void do_hdel(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != ObjType::Hash) return out_err(out, ErrCode::BadType, "WRONGTYPE not a hash");
    int64_t removed = 0;
    for (size_t i = 2; i < cmd.size(); i++) removed += ent->hash->erase(cmd[i]);
    return out_int(out, removed);
}

static void do_hgetall(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_arr(out, 0);
    if (ent->type != ObjType::Hash) return out_err(out, ErrCode::BadType, "WRONGTYPE not a hash");
    out_arr(out, static_cast<uint32_t>(ent->hash->size() * 2));
    for (auto &kv : *ent->hash) {
        out_str(out, kv.first);
        out_str(out, kv.second);
    }
}

static void do_hlen(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != ObjType::Hash) return out_err(out, ErrCode::BadType, "WRONGTYPE not a hash");
    return out_int(out, ent->hash->size());
}

static void do_sadd(const std::vector<std::string> &cmd, Buffer &out) {
    bool created;
    Entry *ent = get_or_create(cmd[1], ObjType::Set, out, &created);
    if (!ent) return;
    int64_t added = 0;
    for (size_t i = 2; i < cmd.size(); i++) added += ent->set->emplace(cmd[i], 1).second;
    return out_int(out, added);
}

static void do_srem(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != ObjType::Set) return out_err(out, ErrCode::BadType, "WRONGTYPE not a set");
    int64_t removed = 0;
    for (size_t i = 2; i < cmd.size(); i++) removed += ent->set->erase(cmd[i]);
    return out_int(out, removed);
}

static void do_sismember(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != ObjType::Set) return out_err(out, ErrCode::BadType, "WRONGTYPE not a set");
    return out_int(out, ent->set->count(cmd[2]) ? 1 : 0);
}

static void do_smembers(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_arr(out, 0);
    if (ent->type != ObjType::Set) return out_err(out, ErrCode::BadType, "WRONGTYPE not a set");
    out_arr(out, static_cast<uint32_t>(ent->set->size()));
    for (auto &kv : *ent->set) out_str(out, kv.first);
}

static void do_scard(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != ObjType::Set) return out_err(out, ErrCode::BadType, "WRONGTYPE not a set");
    return out_int(out, ent->set->size());
}

static void do_zadd(const std::vector<std::string> &cmd, Buffer &out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) return out_err(out, ErrCode::BadArg, "expect float");
    bool created;
    Entry *ent = get_or_create(cmd[1], ObjType::ZSet, out, &created);
    if (!ent) return;
    bool added = zset_insert(ent->zset, cmd[3].data(), cmd[3].size(), score);
    return out_int(out, added ? 1 : 0);
}

static ZSet *expect_zset(const std::string &s, bool *type_err) {
    *type_err = false;
    Entry *ent = lookup_entry(s);
    if (!ent) return nullptr;
    if (ent->type != ObjType::ZSet) { *type_err = true; return nullptr; }
    return ent->zset;
}

static void do_zrem(const std::vector<std::string> &cmd, Buffer &out) {
    bool type_err;
    ZSet *zset = expect_zset(cmd[1], &type_err);
    if (type_err) return out_err(out, ErrCode::BadType, "WRONGTYPE not a zset");
    if (!zset) return out_int(out, 0);
    ZMember *m = zset_lookup(zset, cmd[2].data(), cmd[2].size());
    if (m) zset_delete(zset, m);
    return out_int(out, m ? 1 : 0);
}

static void do_zscore(const std::vector<std::string> &cmd, Buffer &out) {
    bool type_err;
    ZSet *zset = expect_zset(cmd[1], &type_err);
    if (type_err) return out_err(out, ErrCode::BadType, "WRONGTYPE not a zset");
    if (!zset) return out_nil(out);
    ZMember *m = zset_lookup(zset, cmd[2].data(), cmd[2].size());
    return m ? out_dbl(out, m->score) : out_nil(out);
}

static void do_zquery(const std::vector<std::string> &cmd, Buffer &out) {
    double score = 0;
    int64_t offset = 0, limit = 0;
    if (!str2dbl(cmd[2], score) || !str2int(cmd[4], offset) || !str2int(cmd[5], limit)) {
        return out_err(out, ErrCode::BadArg, "expect number");
    }
    const std::string &name = cmd[3];
    bool type_err;
    ZSet *zset = expect_zset(cmd[1], &type_err);
    if (type_err) return out_err(out, ErrCode::BadType, "WRONGTYPE not a zset");
    if (limit <= 0 || !zset) return out_arr(out, 0);

    SkiplistNode *node = zset_seekge(zset, score, name.data(), name.size());
    node = sl_offset(node, offset);

    size_t ctx = out_begin_arr(out);
    int64_t n = 0;
    while (node && n < limit) {
        out_str(out, node->member);
        out_dbl(out, node->score);
        node = sl_offset(node, +1);
        n += 2;
    }
    out_end_arr(out, ctx, static_cast<uint32_t>(n));
}

// ---- Persistence ----
std::string g_data_dir, g_rdb_path, g_rdb_tmp_path, g_aof_path, g_aof_tmp_path;
constexpr uint32_t k_rdb_magic = 0x42445230;
constexpr uint64_t k_aof_fsync_interval_ms = 1000;
constexpr uint64_t k_aof_rewrite_min_size = 1 << 20;

static void init_paths() {
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string exe_dir = ".";
    if (len != -1) {
        buf[len] = '\0';
        char *last_slash = strrchr(buf, '/');
        if (last_slash) { *last_slash = '\0'; exe_dir = std::string(buf); }
    }
    g_data_dir = exe_dir + "/data";
    g_rdb_path = g_data_dir + "/dump.rdb";
    g_rdb_tmp_path = g_data_dir + "/dump.rdb.tmp";
    g_aof_path = g_data_dir + "/appendonly.aof";
    g_aof_tmp_path = g_data_dir + "/appendonly.aof.tmp";
}

static bool fwrite_u32(FILE *fp, uint32_t v) { return fwrite(&v, 4, 1, fp) == 1; }
static bool fwrite_i64(FILE *fp, int64_t v) { return fwrite(&v, 8, 1, fp) == 1; }
static bool fwrite_dbl(FILE *fp, double v) { return fwrite(&v, 8, 1, fp) == 1; }
static bool fwrite_str(FILE *fp, const std::string &s) {
    if (!fwrite_u32(fp, static_cast<uint32_t>(s.size()))) return false;
    return s.empty() || fwrite(s.data(), 1, s.size(), fp) == s.size();
}

static bool fread_u32(FILE *fp, uint32_t &v) { return fread(&v, 4, 1, fp) == 1; }
static bool fread_i64(FILE *fp, int64_t &v) { return fread(&v, 8, 1, fp) == 1; }
static bool fread_dbl(FILE *fp, double &v) { return fread(&v, 8, 1, fp) == 1; }
static bool fread_str(FILE *fp, std::string &s) {
    uint32_t len = 0;
    if (!fread_u32(fp, len)) return false;
    s.resize(len);
    return len == 0 || fread(&s[0], 1, len, fp) == len;
}

struct SaveCtx { FILE *fp = nullptr; bool ok = true; };

static bool save_entry_cb(HNode *node, void *arg) {
    SaveCtx *ctx = static_cast<SaveCtx *>(arg);
    if (!ctx->ok) return false;
    Entry *ent = container_of(node, Entry, node);
    FILE *fp = ctx->fp;

    int64_t ttl_ms = -1;
    if (ent->heap_idx != static_cast<size_t>(-1)) {
        uint64_t expire_at = g_server.heap[ent->heap_idx].val;
        uint64_t now_ms = get_monotonic_msec();
        ttl_ms = expire_at > now_ms ? static_cast<int64_t>(expire_at - now_ms) : 0;
    }

    bool ok = fwrite_str(fp, ent->key) && fwrite_u32(fp, static_cast<uint32_t>(ent->type)) && fwrite_i64(fp, ttl_ms);

    switch (ent->type) {
        case ObjType::Str: ok = ok && fwrite_str(fp, *ent->str); break;
        case ObjType::List:
            ok = ok && fwrite_u32(fp, static_cast<uint32_t>(ent->list->size()));
            for (auto &v : *ent->list) ok = ok && fwrite_str(fp, v);
            break;
        case ObjType::Hash:
            ok = ok && fwrite_u32(fp, static_cast<uint32_t>(ent->hash->size()));
            for (auto &kv : *ent->hash) ok = ok && fwrite_str(fp, kv.first) && fwrite_str(fp, kv.second);
            break;
        case ObjType::Set:
            ok = ok && fwrite_u32(fp, static_cast<uint32_t>(ent->set->size()));
            for (auto &kv : *ent->set) ok = ok && fwrite_str(fp, kv.first);
            break;
        case ObjType::ZSet: {
            uint64_t n = ent->zset->sl ? ent->zset->sl->length : 0;
            ok = ok && fwrite_u32(fp, static_cast<uint32_t>(n));
            if (ok && ent->zset->sl) {
                for (SkiplistNode *sn = ent->zset->sl->header->level[0].forward; ok && sn; sn = sn->level[0].forward) {
                    ok = fwrite_str(fp, sn->member) && fwrite_dbl(fp, sn->score);
                }
            }
            break;
        }
        default: break;
    }
    ctx->ok = ok;
    return ok;
}

static bool save_snapshot(const char *path) {
    FILE *fp = fopen(g_rdb_tmp_path.c_str(), "wb");
    if (!fp) return false;
    bool ok = fwrite_u32(fp, k_rdb_magic) && fwrite_i64(fp, static_cast<int64_t>(hm_size(&g_server.db)));
    SaveCtx ctx{fp, ok};
    if (ok) { hm_foreach(&g_server.db, &save_entry_cb, &ctx); ok = ctx.ok; }
    ok = ok && (fflush(fp) == 0) && (fsync(fileno(fp)) == 0);
    fclose(fp);
    if (!ok) { unlink(g_rdb_tmp_path.c_str()); return false; }
    if (rename(g_rdb_tmp_path.c_str(), path) != 0) { unlink(g_rdb_tmp_path.c_str()); return false; }
    return true;
}

static bool load_entry(FILE *fp) {
    std::string key; uint32_t type = 0; int64_t ttl_ms = -1;
    if (!fread_str(fp, key) || !fread_u32(fp, type) || !fread_i64(fp, ttl_ms)) return false;
    ObjType objType = static_cast<ObjType>(type);
    Entry *ent = entry_new(objType);
    ent->key = key; ent->node.hcode = str_hash(reinterpret_cast<const uint8_t*>(key.data()), key.size());

    bool ok = true;
    switch (objType) {
        case ObjType::Str: ok = fread_str(fp, *ent->str); break;
        case ObjType::List: {
            uint32_t n = 0; ok = fread_u32(fp, n);
            for (uint32_t i = 0; ok && i < n; i++) { std::string v; ok = fread_str(fp, v); if (ok) ent->list->push_back(std::move(v)); }
            break;
        }
        case ObjType::Hash: {
            uint32_t n = 0; ok = fread_u32(fp, n);
            for (uint32_t i = 0; ok && i < n; i++) { std::string k, v; ok = fread_str(fp, k) && fread_str(fp, v); if (ok) (*ent->hash)[k] = v; }
            break;
        }
        case ObjType::Set: {
            uint32_t n = 0; ok = fread_u32(fp, n);
            for (uint32_t i = 0; ok && i < n; i++) { std::string m; ok = fread_str(fp, m); if (ok) (*ent->set)[m] = 1; }
            break;
        }
        case ObjType::ZSet: {
            uint32_t n = 0; ok = fread_u32(fp, n);
            for (uint32_t i = 0; ok && i < n; i++) { std::string m; double score = 0; ok = fread_str(fp, m) && fread_dbl(fp, score); if (ok) zset_insert(ent->zset, m.data(), m.size(), score); }
            break;
        }
        default: break;
    }
    if (!ok) { entry_del_sync(ent); return false; }
    hm_insert(&g_server.db, &ent->node);
    if (ttl_ms >= 0) entry_set_ttl(ent, ttl_ms);
    return true;
}

static bool load_snapshot(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    uint32_t magic = 0; int64_t nentries = 0;
    if (!fread_u32(fp, magic) || magic != k_rdb_magic || !fread_i64(fp, nentries) || nentries < 0) { fclose(fp); return false; }
    for (int64_t i = 0; i < nentries; i++) if (!load_entry(fp)) break;
    fclose(fp);
    return true;
}

static bool is_write_command(const std::string &name) {
    static const std::unordered_map<std::string, bool> k_write_cmds = {
        {"set", true}, {"del", true}, {"pexpire", true}, {"lpush", true}, {"rpush", true},
        {"lpop", true}, {"rpop", true}, {"hset", true}, {"hdel", true}, {"sadd", true},
        {"srem", true}, {"zadd", true}, {"zrem", true}
    };
    return k_write_cmds.count(name) != 0;
}

static void aof_encode_cmd(const std::vector<std::string> &cmd, Buffer &buf) {
    buf_append_u32(buf, static_cast<uint32_t>(cmd.size()));
    for (const std::string &s : cmd) {
        buf_append_u32(buf, static_cast<uint32_t>(s.size()));
        buf_append(buf, reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
}

static bool aof_write_frame(FILE *fp, const std::vector<std::string> &cmd, uint64_t *size_out) {
    Buffer payload; aof_encode_cmd(cmd, payload);
    uint32_t len = static_cast<uint32_t>(payload.size());
    bool ok = fwrite(&len, 4, 1, fp) == 1;
    ok = ok && (payload.empty() || fwrite(payload.data(), 1, payload.size(), fp) == payload.size());
    if (ok && size_out) *size_out += 4 + payload.size();
    return ok;
}

static void aof_append(const std::vector<std::string> &cmd) {
    if (!g_server.aof_fp) return;
    aof_write_frame(g_server.aof_fp, cmd, &g_server.aof_size);
    fflush(g_server.aof_fp);
    g_server.aof_dirty_since_fsync = true;
}

struct AofRewriteCtx { FILE *fp = nullptr; uint64_t size = 0; bool ok = true; };

static bool aof_rewrite_entry_cb(HNode *node, void *arg) {
    AofRewriteCtx *ctx = static_cast<AofRewriteCtx *>(arg);
    if (!ctx->ok) return false;
    Entry *ent = container_of(node, Entry, node);
    auto emit = [&](std::vector<std::string> cmd) { if (ctx->ok) ctx->ok = aof_write_frame(ctx->fp, cmd, &ctx->size); };

    switch (ent->type) {
        case ObjType::Str: emit({"set", ent->key, *ent->str}); break;
        case ObjType::List:
            if (!ent->list->empty()) { std::vector<std::string> cmd = {"rpush", ent->key}; for (auto &v : *ent->list) cmd.push_back(v); emit(cmd); }
            break;
        case ObjType::Hash:
            if (!ent->hash->empty()) { std::vector<std::string> cmd = {"hset", ent->key}; for (auto &kv : *ent->hash) { cmd.push_back(kv.first); cmd.push_back(kv.second); } emit(cmd); }
            break;
        case ObjType::Set:
            if (!ent->set->empty()) { std::vector<std::string> cmd = {"sadd", ent->key}; for (auto &kv : *ent->set) cmd.push_back(kv.first); emit(cmd); }
            break;
        case ObjType::ZSet:
            if (ent->zset->sl) {
                for (SkiplistNode *sn = ent->zset->sl->header->level[0].forward; ctx->ok && sn; sn = sn->level[0].forward) {
                    char scorebuf[32]; snprintf(scorebuf, sizeof(scorebuf), "%.17g", sn->score);
                    emit({"zadd", ent->key, scorebuf, sn->member});
                }
            }
            break;
        default: break;
    }
    if (ctx->ok && ent->heap_idx != static_cast<size_t>(-1)) {
        uint64_t expire_at = g_server.heap[ent->heap_idx].val;
        int64_t ttl_ms = expire_at > get_monotonic_msec() ? static_cast<int64_t>(expire_at - get_monotonic_msec()) : 0;
        char ttlbuf[24]; snprintf(ttlbuf, sizeof(ttlbuf), "%lld", (long long)ttl_ms);
        emit({"pexpire", ent->key, ttlbuf});
    }
    return ctx->ok;
}

static bool aof_rewrite() {
    FILE *fp = fopen(g_aof_tmp_path.c_str(), "wb");
    if (!fp) return false;
    AofRewriteCtx ctx{fp, 0, true};
    hm_foreach(&g_server.db, &aof_rewrite_entry_cb, &ctx);
    bool ok = ctx.ok && (fflush(fp) == 0) && (fsync(fileno(fp)) == 0);
    fclose(fp);
    if (!ok) { unlink(g_aof_tmp_path.c_str()); return false; }
    if (rename(g_aof_tmp_path.c_str(), g_aof_path.c_str()) != 0) { unlink(g_aof_tmp_path.c_str()); return false; }
    if (g_server.aof_fp) fclose(g_server.aof_fp);
    g_server.aof_fp = fopen(g_aof_path.c_str(), "ab");
    if (!g_server.aof_fp) return false;
    g_server.aof_size = ctx.size;
    g_server.aof_size_at_last_rewrite = ctx.size;
    g_server.aof_dirty_since_fsync = false;
    return true;
}

static void do_request(const std::vector<std::string> &cmd, Buffer &out);

static bool aof_load(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    while (true) {
        uint32_t len = 0; if (fread(&len, 4, 1, fp) != 1) break;
        std::vector<uint8_t> payload(len);
        if (len > 0 && fread(payload.data(), 1, len, fp) != len) break;
        std::vector<std::string> cmd;
        if (parse_req(payload.data(), len, cmd) < 0 || cmd.empty()) break;
        Buffer discard; do_request(cmd, discard);
    }
    fclose(fp);
    return true;
}

static void do_stats(const std::vector<std::string> &, Buffer &out) {
    uint64_t uptime_ms = get_monotonic_msec() - g_server.start_time_ms;
    long rss_kb = get_rss_kb();
    size_t ctx = out_begin_arr(out); uint32_t n = 0;
    auto pair = [&](const char *k, int64_t v) { out_str(out, k, strlen(k)); out_int(out, v); n += 2; };
    pair("connections", static_cast<int64_t>(g_server.active_conns));
    pair("total_connections", static_cast<int64_t>(g_server.total_conns));
    pair("ops_per_sec", static_cast<int64_t>(g_server.ops_per_sec));
    pair("total_ops", static_cast<int64_t>(g_server.total_cmds));
    pair("keys", static_cast<int64_t>(hm_size(&g_server.db)));
    pair("uptime_ms", static_cast<int64_t>(uptime_ms));
    pair("memory_kb", rss_kb >= 0 ? rss_kb : 0);
    pair("aof_enabled", g_server.aof_fp ? 1 : 0);
    pair("aof_size_bytes", static_cast<int64_t>(g_server.aof_size));
    out_end_arr(out, ctx, n);
}

static void do_type(const std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_str(out, "none", 4);
    return out_str(out, type_name(ent->type), strlen(type_name(ent->type)));
}

static void do_save(const std::vector<std::string> &, Buffer &out) {
    bool ok = save_snapshot(g_rdb_path.c_str());
    if (ok) g_server.ops_since_save = 0;
    return out_int(out, ok ? 1 : 0);
}

static void do_bgrewriteaof(const std::vector<std::string> &, Buffer &out) {
    return out_int(out, aof_rewrite() ? 1 : 0);
}

using CommandHandler = void (*)(const std::vector<std::string>&, Buffer&);
struct CommandDef { size_t min_args; CommandHandler handler; };

static const std::unordered_map<std::string, CommandDef> k_commands = {
    {"get", {2, do_get}}, {"set", {3, do_set}}, {"del", {2, do_del}},
    {"pexpire", {3, do_pexpire}}, {"pttl", {2, do_pttl}}, {"keys", {1, do_keys}},
    {"lpush", {3, do_lpush}}, {"rpush", {3, do_rpush}}, {"lpop", {2, do_lpop}},
    {"rpop", {2, do_rpop}}, {"llen", {2, do_llen}}, {"lrange", {4, do_lrange}},
    {"hset", {4, do_hset}}, {"hget", {3, do_hget}}, {"hdel", {3, do_hdel}},
    {"hgetall", {2, do_hgetall}}, {"hlen", {2, do_hlen}},
    {"sadd", {3, do_sadd}}, {"srem", {3, do_srem}}, {"sismember", {3, do_sismember}},
    {"smembers", {2, do_smembers}}, {"scard", {2, do_scard}},
    {"zadd", {4, do_zadd}}, {"zrem", {3, do_zrem}}, {"zscore", {3, do_zscore}},
    {"zquery", {6, do_zquery}},
    {"stats", {1, do_stats}}, {"type", {2, do_type}}, {"save", {1, do_save}},
    {"bgrewriteaof", {1, do_bgrewriteaof}}
};

static void do_request(const std::vector<std::string> &cmd, Buffer &out) {
    if (cmd.empty()) return out_err(out, ErrCode::Unknown, "empty command");
    auto it = k_commands.find(cmd[0]);
    if (it != k_commands.end()) {
        if (cmd.size() < it->second.min_args) return out_err(out, ErrCode::BadArg, "wrong number of arguments");
        it->second.handler(cmd, out);
    } else {
        out_err(out, ErrCode::Unknown, "unknown command");
    }
}

static void response_begin(Buffer &out, size_t *header) { *header = out.size(); buf_append_u32(out, 0); }
static size_t response_size(Buffer &out, size_t header) { return out.size() - header - 4; }
static void response_end(Buffer &out, size_t header) {
    size_t msg_size = response_size(out, header);
    if (msg_size > k_max_msg) { out.resize(header + 4); out_err(out, ErrCode::TooBig, "response too big"); msg_size = response_size(out, header); }
    uint32_t len = static_cast<uint32_t>(msg_size);
    memcpy(&out[header], &len, 4);
}

static bool try_one_request(Conn *conn) {
    if (conn->incoming.size() < 4) return false;
    uint32_t len = 0; memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) { conn->want_close = true; return false; }
    if (4 + len > conn->incoming.size()) return false;

    std::vector<std::string> cmd;
    if (parse_req(&conn->incoming[4], len, cmd) < 0) { conn->want_close = true; return false; }

    size_t header_pos = 0;
    response_begin(conn->outgoing, &header_pos);
    do_request(cmd, conn->outgoing);
    if (!cmd.empty() && is_write_command(cmd[0]) && conn->outgoing[header_pos + 4] != static_cast<uint8_t>(RespTag::Err)) {
        aof_append(cmd);
    }
    response_end(conn->outgoing, header_pos);
    g_server.total_cmds++; g_server.cmds_this_second++; g_server.ops_since_save++;
    buf_consume(conn->incoming, 4 + len);
    return true;
}

static void handle_write(Conn *conn) {
    while (!conn->outgoing.empty()) {
        ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
        if (rv < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; conn->want_close = true; return; }
        buf_consume(conn->outgoing, static_cast<size_t>(rv));
    }
    conn->want_read = conn->outgoing.empty();
    conn->want_write = !conn->outgoing.empty();
    conn_update_epoll(conn);
}

static void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024];
    while (true) {
        ssize_t rv = read(conn->fd, buf, sizeof(buf));
        if (rv < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) break; conn->want_close = true; return; }
        if (rv == 0) { conn->want_close = true; break; }
        buf_append(conn->incoming, buf, static_cast<size_t>(rv));
        if (static_cast<size_t>(rv) < sizeof(buf)) break;
    }
    while (try_one_request(conn)) {}
    if (!conn->outgoing.empty()) { conn->want_read = false; conn->want_write = true; handle_write(conn); return; }
    conn_update_epoll(conn);
}

constexpr uint64_t k_idle_timeout_ms = 5 * 1000;
constexpr uint64_t k_save_interval_ms = 60 * 1000;

static int32_t next_timer_ms() {
    uint64_t now_ms = get_monotonic_msec();
    uint64_t next_ms = static_cast<uint64_t>(-1);
    if (!dlist_empty(&g_server.idle_list)) {
        Conn *conn = container_of(g_server.idle_list.next, Conn, idle_node);
        next_ms = conn->last_active_ms + k_idle_timeout_ms;
    }
    if (!g_server.heap.empty() && g_server.heap[0].val < next_ms) next_ms = g_server.heap[0].val;
    if (g_server.last_stats_ms + 1000 < next_ms) next_ms = g_server.last_stats_ms + 1000;
    if (g_server.last_save_ms + k_save_interval_ms < next_ms) next_ms = g_server.last_save_ms + k_save_interval_ms;
    if (g_server.aof_fp && g_server.last_aof_fsync_ms + k_aof_fsync_interval_ms < next_ms) next_ms = g_server.last_aof_fsync_ms + k_aof_fsync_interval_ms;
    if (next_ms == static_cast<uint64_t>(-1)) return -1;
    if (next_ms <= now_ms) return 0;
    return static_cast<int32_t>(next_ms - now_ms);
}

static bool hnode_same(HNode *node, HNode *key) { return node == key; }

static void process_timers() {
    uint64_t now_ms = get_monotonic_msec();
    if (now_ms - g_server.last_stats_ms >= 1000) {
        g_server.ops_per_sec = g_server.cmds_this_second;
        g_server.cmds_this_second = 0;
        g_server.last_stats_ms = now_ms;
    }
    while (!dlist_empty(&g_server.idle_list)) {
        Conn *conn = container_of(g_server.idle_list.next, Conn, idle_node);
        if (conn->last_active_ms + k_idle_timeout_ms >= now_ms) break;
        conn_destroy(conn);
    }
    constexpr size_t k_max_works = 2000; size_t nworks = 0;
    while (!g_server.heap.empty() && g_server.heap[0].val < now_ms) {
        Entry *ent = container_of(g_server.heap[0].ref, Entry, heap_idx);
        hm_delete(&g_server.db, &ent->node, &hnode_same);
        entry_del(ent);
        if (nworks++ >= k_max_works) break;
    }
    if (now_ms - g_server.last_save_ms >= k_save_interval_ms) {
        if (g_server.ops_since_save > 0) { if (save_snapshot(g_rdb_path.c_str())) g_server.ops_since_save = 0; }
        g_server.last_save_ms = now_ms;
    }
    if (g_server.aof_fp && now_ms - g_server.last_aof_fsync_ms >= k_aof_fsync_interval_ms) {
        if (g_server.aof_dirty_since_fsync) { fsync(fileno(g_server.aof_fp)); g_server.aof_dirty_since_fsync = false; }
        g_server.last_aof_fsync_ms = now_ms;
        if (g_server.aof_size >= k_aof_rewrite_min_size && g_server.aof_size >= g_server.aof_size_at_last_rewrite * 2) {
            aof_rewrite();
        }
    }
}

static volatile sig_atomic_t g_shutdown_requested = 0;
static void on_shutdown_signal(int) { g_shutdown_requested = 1; }

int main() {
    init_paths();
    mkdir(g_data_dir.c_str(), 0755);
    dlist_init(&g_server.idle_list);
    g_server.thread_pool.init(4);
    g_server.start_time_ms = get_monotonic_msec();
    g_server.last_stats_ms = g_server.start_time_ms;
    g_server.last_save_ms = g_server.start_time_ms;
    g_server.last_aof_fsync_ms = g_server.start_time_ms;

    struct stat aof_st;
    if (stat(g_aof_path.c_str(), &aof_st) == 0 && aof_st.st_size > 0) {
        aof_load(g_aof_path.c_str());
    } else {
        load_snapshot(g_rdb_path.c_str());
    }

    g_server.aof_fp = fopen(g_aof_path.c_str(), "ab");
    if (g_server.aof_fp) {
        struct stat st;
        g_server.aof_size = (fstat(fileno(g_server.aof_fp), &st) == 0) ? static_cast<uint64_t>(st.st_size) : 0;
        g_server.aof_size_at_last_rewrite = g_server.aof_size;
    }

    struct sigaction sa = {}; sa.sa_handler = on_shutdown_signal;
    sigaction(SIGINT, &sa, nullptr); sigaction(SIGTERM, &sa, nullptr);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");
    int val = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET; addr.sin_port = htons(1234); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr))) die("bind()");
    fd_set_nb(fd);
    if (listen(fd, SOMAXCONN)) die("listen()");

    g_server.epfd = epoll_create1(0);
    if (g_server.epfd < 0) die("epoll_create1()");

    struct epoll_event listen_ev = {}; listen_ev.events = EPOLLIN; listen_ev.data.fd = fd;
    if (epoll_ctl(g_server.epfd, EPOLL_CTL_ADD, fd, &listen_ev) < 0) die("epoll_ctl(listen)");

    constexpr int k_max_events = 256;
    std::vector<struct epoll_event> events(k_max_events);

    while (true) {
        if (g_shutdown_requested) {
            save_snapshot(g_rdb_path.c_str());
            if (g_server.aof_fp) { fflush(g_server.aof_fp); fsync(fileno(g_server.aof_fp)); }
            break;
        }

        int32_t timeout_ms = next_timer_ms();
        int n = epoll_wait(g_server.epfd, events.data(), k_max_events, timeout_ms);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) die("epoll_wait");

        for (int i = 0; i < n; i++) {
            int evfd = events[i].data.fd; uint32_t flags = events[i].events;
            if (evfd == fd) { while (handle_accept(fd) == 0) {} continue; }

            Conn *conn = g_server.fd2conn[evfd];
            if (!conn) continue;

            conn->last_active_ms = get_monotonic_msec();
            dlist_detach(&conn->idle_node);
            dlist_insert_before(&g_server.idle_list, &conn->idle_node);

            if (flags & (EPOLLHUP | EPOLLERR)) {
                conn->want_close = true;
            } else {
                if (flags & EPOLLIN) handle_read(conn);
                if (!conn->want_close && (flags & EPOLLOUT)) handle_write(conn);
                if (!conn->want_close && (flags & EPOLLRDHUP)) conn->want_close = true;
            }
            if (conn->want_close) conn_destroy(conn);
        }
        process_timers();
    }
    return 0;
}