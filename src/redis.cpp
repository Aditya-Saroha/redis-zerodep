#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#pragma region common

#define container_of(ptr, type, member) ({ \
    const typeof(((type *)0)->member) *__mptr = (ptr); \
    (type *)((char *)__mptr - offsetof(type, member)); })

inline uint64_t str_hash(const uint8_t *data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

#pragma endregion

#pragma region hashtable
// Open-chaining hashtable with progressive (incremental) rehashing, same
// idea as Redis's own dict.c: growing the table all at once would stall
// the event loop on a big table, so a resize instead allocates the new
// (larger) table and migrates a bounded number of buckets per subsequent
// insert/lookup/delete, until the old table is empty and freed. Used for
// the top-level keyspace and for sorted-set member lookup. Nested Hash/Set
// field storage further down uses std::unordered_map instead, since those
// don't grow to keyspace scale.

struct HNode {
    HNode *next = NULL;
    uint64_t hcode = 0;
};

struct HTab {
    HNode **tab = NULL;
    size_t mask = 0;
    size_t size = 0;
};

struct HMap {
    HTab newer;
    HTab older;
    size_t migrate_pos = 0;
};

static void h_init(HTab *htab, size_t n) {
    assert(n > 0 && ((n - 1) & n) == 0);
    htab->tab = (HNode **)calloc(n, sizeof(HNode *));
    htab->mask = n - 1;
    htab->size = 0;
}

static void h_insert(HTab *htab, HNode *node) {
    size_t pos = node->hcode & htab->mask;
    HNode *next = htab->tab[pos];
    node->next = next;
    htab->tab[pos] = node;
    htab->size++;
}

static HNode **h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *)) {
    if (!htab->tab) {
        return NULL;
    }

    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];
    for (HNode *cur; (cur = *from) != NULL; from = &cur->next) {
        if (cur->hcode == key->hcode && eq(cur, key)) {
            return from;
        }
    }
    return NULL;
}

static HNode *h_detach(HTab *htab, HNode **from) {
    HNode *node = *from;
    *from = node->next;
    htab->size--;
    return node;
}

const size_t k_rehashing_work = 128;

static void hm_help_rehashing(HMap *hmap) {
    size_t nwork = 0;
    while (nwork < k_rehashing_work && hmap->older.size > 0) {
        HNode **from = &hmap->older.tab[hmap->migrate_pos];
        if (!*from) {
            hmap->migrate_pos++;
            continue;
        }
        h_insert(&hmap->newer, h_detach(&hmap->older, from));
        nwork++;
    }
    if (hmap->older.size == 0 && hmap->older.tab) {
        free(hmap->older.tab);
        hmap->older = HTab{};
    }
}

static void hm_trigger_rehashing(HMap *hmap) {
    assert(hmap->older.tab == NULL);
    hmap->older = hmap->newer;
    h_init(&hmap->newer, (hmap->newer.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);
    HNode **from = h_lookup(&hmap->newer, key, eq);
    if (!from) {
        from = h_lookup(&hmap->older, key, eq);
    }
    return from ? *from : NULL;
}

const size_t k_max_load_factor = 8;

void hm_insert(HMap *hmap, HNode *node) {
    if (!hmap->newer.tab) {
        h_init(&hmap->newer, 4);
    }
    h_insert(&hmap->newer, node);

    if (!hmap->older.tab) {
        size_t shreshold = (hmap->newer.mask + 1) * k_max_load_factor;
        if (hmap->newer.size >= shreshold) {
            hm_trigger_rehashing(hmap);
        }
    }
    hm_help_rehashing(hmap);
}

HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);
    if (HNode **from = h_lookup(&hmap->newer, key, eq)) {
        return h_detach(&hmap->newer, from);
    }
    if (HNode **from = h_lookup(&hmap->older, key, eq)) {
        return h_detach(&hmap->older, from);
    }
    return NULL;
}

void hm_clear(HMap *hmap) {
    free(hmap->newer.tab);
    free(hmap->older.tab);
    *hmap = HMap{};
}

size_t hm_size(HMap *hmap) {
    return hmap->newer.size + hmap->older.size;
}

static bool h_foreach(HTab *htab, bool (*f)(HNode *, void *), void *arg) {
    for (size_t i = 0; htab->mask != 0 && i <= htab->mask; i++) {
        for (HNode *node = htab->tab[i]; node != NULL; node = node->next) {
            if (!f(node, arg)) {
                return false;
            }
        }
    }
    return true;
}

void hm_foreach(HMap *hmap, bool (*f)(HNode *, void *), void *arg) {
    h_foreach(&hmap->newer, f, arg) && h_foreach(&hmap->older, f, arg);
}

#pragma endregion

#pragma region heap
// Binary min-heap of expiry timestamps, used for TTL eviction. Each
// HeapItem carries a `ref` back to the owning Entry's heap_idx, so the
// heap can be reordered without a separate index structure.

struct HeapItem {
    uint64_t val = 0;
    size_t *ref = NULL;
};

static size_t heap_parent(size_t i) { return (i + 1) / 2 - 1; }
static size_t heap_left(size_t i) { return i * 2 + 1; }
static size_t heap_right(size_t i) { return i * 2 + 2; }

static void heap_up(HeapItem *a, size_t pos) {
    HeapItem t = a[pos];
    while (pos > 0 && a[heap_parent(pos)].val > t.val) {
        a[pos] = a[heap_parent(pos)];
        *a[pos].ref = pos;
        pos = heap_parent(pos);
    }
    a[pos] = t;
    *a[pos].ref = pos;
}

static void heap_down(HeapItem *a, size_t pos, size_t len) {
    HeapItem t = a[pos];
    while (true) {
        size_t l = heap_left(pos);
        size_t r = heap_right(pos);
        size_t min_pos = pos;
        uint64_t min_val = t.val;
        if (l < len && a[l].val < min_val) {
            min_pos = l;
            min_val = a[l].val;
        }
        if (r < len && a[r].val < min_val) {
            min_pos = r;
        }
        if (min_pos == pos) {
            break;
        }
        a[pos] = a[min_pos];
        *a[pos].ref = pos;
        pos = min_pos;
    }
    a[pos] = t;
    *a[pos].ref = pos;
}

void heap_update(HeapItem *a, size_t pos, size_t len) {
    if (pos > 0 && a[heap_parent(pos)].val > a[pos].val) {
        heap_up(a, pos);
    } else {
        heap_down(a, pos, len);
    }
}

#pragma endregion

#pragma region dlist
// Intrusive doubly linked list, used only for idle-connection tracking.

struct DList {
    DList *prev = NULL;
    DList *next = NULL;
};

inline void dlist_init(DList *node) { node->prev = node->next = node; }
inline bool dlist_empty(DList *node) { return node->next == node; }

inline void dlist_detach(DList *node) {
    DList *prev = node->prev;
    DList *next = node->next;
    prev->next = next;
    next->prev = prev;
}

inline void dlist_insert_before(DList *target, DList *rookie) {
    DList *prev = target->prev;
    prev->next = rookie;
    rookie->prev = prev;
    rookie->next = target;
    target->prev = rookie;
}

#pragma endregion

#pragma region thread_pool
// Used only to offload deleting very large containers off the event-loop
// thread, so a big DEL or expiry doesn't stall every other client.

struct Work {
    void (*f)(void *) = NULL;
    void *arg = NULL;
};

struct TheadPool {
    std::vector<pthread_t> threads;
    std::deque<Work> queue;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
};

static void *worker(void *arg) {
    TheadPool *tp = (TheadPool *)arg;
    while (true) {
        pthread_mutex_lock(&tp->mu);
        while (tp->queue.empty()) {
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        }
        Work w = tp->queue.front();
        tp->queue.pop_front();
        pthread_mutex_unlock(&tp->mu);
        w.f(w.arg);
    }
    return NULL;
}

void thread_pool_init(TheadPool *tp, size_t num_threads) {
    assert(num_threads > 0);
    int rv = pthread_mutex_init(&tp->mu, NULL);
    assert(rv == 0);
    rv = pthread_cond_init(&tp->not_empty, NULL);
    assert(rv == 0);
    tp->threads.resize(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        int rv = pthread_create(&tp->threads[i], NULL, &worker, tp);
        assert(rv == 0);
    }
}

void thread_pool_queue(TheadPool *tp, void (*f)(void *), void *arg) {
    pthread_mutex_lock(&tp->mu);
    tp->queue.push_back(Work{f, arg});
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
}

#pragma endregion

#pragma region skiplist
// Skiplist backing sorted sets — the same structure Redis itself uses for
// ZSETs (t_zset.c): a multi-level linked list whose forward pointers are
// each annotated with a "span" (how many nodes that pointer skips), so
// rank-based access (ZQUERY's offset, what would be ZRANK) stays O(log n)
// without the rebalancing a balanced tree needs. Expected O(log n)
// insert/delete/search; randomized rather than guaranteed, since level
// height is chosen by coin flip rather than derived from tree balance.

#define SKIPLIST_MAXLEVEL 32
#define SKIPLIST_P 0.25

struct SkiplistNode {
    std::string member;
    double score = 0;
    SkiplistNode *backward = NULL;
    struct Level {
        SkiplistNode *forward = NULL;
        uint64_t span = 0;
    };
    std::vector<Level> level;
};

struct Skiplist {
    SkiplistNode *header = NULL;
    SkiplistNode *tail = NULL;
    int level = 1;
    uint64_t length = 0;
};

static SkiplistNode *sl_new_node(int lvl, double score, const std::string &member) {
    SkiplistNode *n = new SkiplistNode();
    n->score = score;
    n->member = member;
    n->level.resize(lvl);
    return n;
}

static int sl_random_level() {
    int lvl = 1;
    while (lvl < SKIPLIST_MAXLEVEL && (rand() & 0xFFFF) < (int)(SKIPLIST_P * 0xFFFF)) {
        lvl++;
    }
    return lvl;
}

static Skiplist *sl_create() {
    Skiplist *sl = new Skiplist();
    sl->header = sl_new_node(SKIPLIST_MAXLEVEL, -HUGE_VAL, "");
    sl->level = 1;
    sl->length = 0;
    return sl;
}

static void sl_free(Skiplist *sl) {
    SkiplistNode *node = sl->header->level[0].forward;
    while (node) {
        SkiplistNode *next = node->level[0].forward;
        delete node;
        node = next;
    }
    delete sl->header;
    delete sl;
}

static inline bool sl_less(double s1, const std::string &m1, double s2, const std::string &m2) {
    if (s1 != s2) {
        return s1 < s2;
    }
    return m1 < m2;
}

static SkiplistNode *sl_insert(Skiplist *sl, double score, const std::string &member) {
    SkiplistNode *update[SKIPLIST_MAXLEVEL];
    uint64_t rank[SKIPLIST_MAXLEVEL];
    SkiplistNode *x = sl->header;
    for (int i = sl->level - 1; i >= 0; i--) {
        rank[i] = (i == sl->level - 1) ? 0 : rank[i + 1];
        while (x->level[i].forward &&
               sl_less(x->level[i].forward->score, x->level[i].forward->member, score, member)) {
            rank[i] += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    int lvl = sl_random_level();
    if (lvl > sl->level) {
        for (int i = sl->level; i < lvl; i++) {
            rank[i] = 0;
            update[i] = sl->header;
            update[i]->level[i].span = sl->length;
        }
        sl->level = lvl;
    }

    SkiplistNode *node = sl_new_node(lvl, score, member);
    for (int i = 0; i < lvl; i++) {
        node->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = node;
        node->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }
    for (int i = lvl; i < sl->level; i++) {
        update[i]->level[i].span++;
    }

    node->backward = (update[0] == sl->header) ? NULL : update[0];
    if (node->level[0].forward) {
        node->level[0].forward->backward = node;
    } else {
        sl->tail = node;
    }
    sl->length++;
    return node;
}

static void sl_delete_node(Skiplist *sl, SkiplistNode *update[], SkiplistNode *x) {
    for (int i = 0; i < sl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            update[i]->level[i].span--;
        }
    }
    if (x->level[0].forward) {
        x->level[0].forward->backward = x->backward;
    } else {
        sl->tail = x->backward;
    }
    while (sl->level > 1 && !sl->header->level[sl->level - 1].forward) {
        sl->level--;
    }
    sl->length--;
}

static bool sl_delete(Skiplist *sl, double score, const std::string &member) {
    SkiplistNode *update[SKIPLIST_MAXLEVEL];
    SkiplistNode *x = sl->header;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               sl_less(x->level[i].forward->score, x->level[i].forward->member, score, member)) {
            x = x->level[i].forward;
        }
        update[i] = x;
    }
    x = x->level[0].forward;
    if (x && x->score == score && x->member == member) {
        sl_delete_node(sl, update, x);
        delete x;
        return true;
    }
    return false;
}

// First node with (score, member) >= the given key. O(log n).
static SkiplistNode *sl_first_ge(Skiplist *sl, double score, const std::string &member) {
    SkiplistNode *x = sl->header;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               sl_less(x->level[i].forward->score, x->level[i].forward->member, score, member)) {
            x = x->level[i].forward;
        }
    }
    return x->level[0].forward;
}

// Walk `offset` positions forward (or backward, if negative) from `node`.
// Same approach real Redis uses for ZRANGEBYSCORE's LIMIT offset: a plain
// level[0]/backward walk, O(offset) rather than a rank re-descent from the
// header. Fine for the paging-sized offsets ZQUERY is used for.
static SkiplistNode *sl_offset(SkiplistNode *node, int64_t offset) {
    while (node && offset > 0) {
        node = node->level[0].forward;
        offset--;
    }
    while (node && offset < 0) {
        node = node->backward;
        offset++;
    }
    return node;
}

#pragma endregion

#pragma region zset
// A sorted set is a skiplist (score-ordered, ranked access) paired with a
// hashmap (O(1) ZSCORE / member lookup) — Redis's own zset representation,
// minus the small-set listpack encoding Redis uses below a size threshold.

struct ZSet {
    Skiplist *sl = NULL;
    HMap hmap; // member name -> ZMember (below)
};

struct ZMember {
    HNode hmap;
    std::string name;
    double score = 0;
};

static void zset_init(ZSet *zset) {
    zset->sl = sl_create();
    zset->hmap = HMap{};
}

struct ZKey {
    HNode node;
    const char *name = NULL;
    size_t len = 0;
};

static bool zmember_eq(HNode *node, HNode *key) {
    ZMember *m = container_of(node, ZMember, hmap);
    ZKey *k = container_of(key, ZKey, node);
    if (m->name.size() != k->len) {
        return false;
    }
    return 0 == memcmp(m->name.data(), k->name, k->len);
}

static ZMember *zset_lookup(ZSet *zset, const char *name, size_t len) {
    if (!zset->sl) {
        return NULL;
    }
    ZKey key;
    key.node.hcode = str_hash((uint8_t *)name, len);
    key.name = name;
    key.len = len;
    HNode *found = hm_lookup(&zset->hmap, &key.node, &zmember_eq);
    return found ? container_of(found, ZMember, hmap) : NULL;
}

// Returns true if this created a new member (vs. updated an existing score).
static bool zset_insert(ZSet *zset, const char *name, size_t len, double score) {
    ZMember *m = zset_lookup(zset, name, len);
    if (m) {
        if (m->score != score) {
            sl_delete(zset->sl, m->score, m->name);
            sl_insert(zset->sl, score, m->name);
            m->score = score;
        }
        return false;
    }
    std::string member(name, len);
    sl_insert(zset->sl, score, member);
    m = new ZMember();
    m->name = member;
    m->score = score;
    m->hmap.hcode = str_hash((uint8_t *)name, len);
    hm_insert(&zset->hmap, &m->hmap);
    return true;
}

static void zset_delete(ZSet *zset, ZMember *m) {
    ZKey key;
    key.node.hcode = m->hmap.hcode;
    key.name = m->name.data();
    key.len = m->name.size();
    HNode *found = hm_delete(&zset->hmap, &key.node, &zmember_eq);
    assert(found);
    sl_delete(zset->sl, m->score, m->name);
    delete container_of(found, ZMember, hmap);
}

static SkiplistNode *zset_seekge(ZSet *zset, double score, const char *name, size_t len) {
    if (!zset->sl) {
        return NULL;
    }
    return sl_first_ge(zset->sl, score, std::string(name, len));
}

static void zset_clear(ZSet *zset) {
    // Free all ZMember hash entries.
    struct Ctx { std::vector<ZMember *> members; };
    Ctx ctx;
    auto collect = [](HNode *node, void *arg) -> bool {
        ((Ctx *)arg)->members.push_back(container_of(node, ZMember, hmap));
        return true;
    };
    hm_foreach(&zset->hmap, collect, &ctx);
    for (ZMember *m : ctx.members) {
        delete m;
    }
    hm_clear(&zset->hmap);
    if (zset->sl) {
        sl_free(zset->sl);
        zset->sl = NULL;
    }
}

#pragma endregion

#if defined(BUILD_SERVER)

using Buffer = std::vector<uint8_t>;

static void msg(const char *msg) { fprintf(stderr, "%s\n", msg); }
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
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }
    flags |= O_NONBLOCK;
    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}

const size_t k_max_msg = 32 << 20;

static void buf_append(Buffer &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

static void buf_consume(Buffer &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

struct Conn {
    int fd = -1;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    // What we last told epoll we're interested in, so we only call
    // epoll_ctl(MOD) when the interest set actually changes.
    uint32_t epoll_events = 0;
    Buffer incoming;
    Buffer outgoing;
    uint64_t last_active_ms = 0;
    DList idle_node;
};

static struct {
    HMap db;
    std::vector<Conn *> fd2conn;
    DList idle_list;
    std::vector<HeapItem> heap;
    TheadPool thread_pool;
    int epfd = -1;
    uint64_t start_time_ms = 0;
    uint64_t active_conns = 0;
    uint64_t total_conns = 0;
    uint64_t total_cmds = 0;
    uint64_t cmds_this_second = 0;
    uint64_t ops_per_sec = 0;
    uint64_t last_stats_ms = 0;
} g_data;

// Recompute which epoll events this Conn wants and push a MOD if it
// changed. Edge-triggered, so we always watch EPOLLIN (to notice new data
// or a close) and add EPOLLOUT only while there's a pending write —
// otherwise epoll_wait would spin returning "writable" forever.
static void conn_update_epoll(Conn *conn) {
    uint32_t events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (conn->want_write) {
        events |= EPOLLOUT;
    }
    if (events == conn->epoll_events) {
        return;
    }
    struct epoll_event ev = {};
    ev.events = events;
    ev.data.fd = conn->fd;
    if (epoll_ctl(g_data.epfd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
        msg_errno("epoll_ctl(MOD) error");
    }
    conn->epoll_events = events;
}

static int32_t handle_accept(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            msg_errno("accept() error");
        }
        return -1;
    }
    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
        ntohs(client_addr.sin_port)
    );

    fd_set_nb(connfd);
    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    conn->last_active_ms = get_monotonic_msec();
    dlist_insert_before(&g_data.idle_list, &conn->idle_node);

    if (g_data.fd2conn.size() <= (size_t)conn->fd) {
        g_data.fd2conn.resize(conn->fd + 1);
    }
    assert(!g_data.fd2conn[conn->fd]);
    g_data.fd2conn[conn->fd] = conn;
    g_data.total_conns++;
    g_data.active_conns++;

    struct epoll_event ev = {};
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    ev.data.fd = conn->fd;
    conn->epoll_events = ev.events;
    if (epoll_ctl(g_data.epfd, EPOLL_CTL_ADD, conn->fd, &ev) < 0) {
        msg_errno("epoll_ctl(ADD) error");
    }
    return 0;
}

static void conn_destroy(Conn *conn) {
    epoll_ctl(g_data.epfd, EPOLL_CTL_DEL, conn->fd, NULL);
    (void)close(conn->fd);
    g_data.fd2conn[conn->fd] = NULL;
    dlist_detach(&conn->idle_node);
    g_data.active_conns--;
    delete conn;
}

const size_t k_max_args = 200 * 1000;

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
        out.push_back(std::string());
        if (!read_str(data, end, len, out.back())) return -1;
    }
    if (data != end) return -1;
    return 0;
}

enum {
    ERR_UNKNOWN = 1,
    ERR_TOO_BIG = 2,
    ERR_BAD_TYP = 3,
    ERR_BAD_ARG = 4,
};

enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

static void buf_append_u8(Buffer &buf, uint8_t data) { buf.push_back(data); }
static void buf_append_u32(Buffer &buf, uint32_t data) { buf_append(buf, (const uint8_t *)&data, 4); }
static void buf_append_i64(Buffer &buf, int64_t data) { buf_append(buf, (const uint8_t *)&data, 8); }
static void buf_append_dbl(Buffer &buf, double data) { buf_append(buf, (const uint8_t *)&data, 8); }

static void out_nil(Buffer &out) { buf_append_u8(out, TAG_NIL); }

static void out_str(Buffer &out, const char *s, size_t size) {
    buf_append_u8(out, TAG_STR);
    buf_append_u32(out, (uint32_t)size);
    buf_append(out, (const uint8_t *)s, size);
}
static void out_str(Buffer &out, const std::string &s) { out_str(out, s.data(), s.size()); }

static void out_int(Buffer &out, int64_t val) {
    buf_append_u8(out, TAG_INT);
    buf_append_i64(out, val);
}

static void out_dbl(Buffer &out, double val) {
    buf_append_u8(out, TAG_DBL);
    buf_append_dbl(out, val);
}

static void out_err(Buffer &out, uint32_t code, const std::string &msg) {
    buf_append_u8(out, TAG_ERR);
    buf_append_u32(out, code);
    buf_append_u32(out, (uint32_t)msg.size());
    buf_append(out, (const uint8_t *)msg.data(), msg.size());
}

static void out_arr(Buffer &out, uint32_t n) {
    buf_append_u8(out, TAG_ARR);
    buf_append_u32(out, n);
}

static size_t out_begin_arr(Buffer &out) {
    out.push_back(TAG_ARR);
    buf_append_u32(out, 0);
    return out.size() - 4;
}

static void out_end_arr(Buffer &out, size_t ctx, uint32_t n) {
    assert(out[ctx - 1] == TAG_ARR);
    memcpy(&out[ctx], &n, 4);
}

// ---- data types -------------------------------------------------------
// Entry allocates only the container its type actually uses (a String
// entry has no live ZSet/list/hash/set pointer, etc.), rather than every
// field being present on every key.

enum {
    T_INIT = 0,
    T_STR = 1,
    T_ZSET = 2,
    T_LIST = 3,
    T_HASH = 4,
    T_SET = 5,
};

struct Entry {
    struct HNode node;
    std::string key;
    size_t heap_idx = (size_t)-1;
    uint32_t type = T_INIT;

    std::string *str = NULL;
    ZSet *zset = NULL;
    std::deque<std::string> *list = NULL;
    std::unordered_map<std::string, std::string> *hash = NULL;
    std::unordered_map<std::string, char> *set = NULL;
};

static const char *type_name(uint32_t t) {
    switch (t) {
        case T_STR: return "string";
        case T_ZSET: return "zset";
        case T_LIST: return "list";
        case T_HASH: return "hash";
        case T_SET: return "set";
        default: return "unknown";
    }
}

static Entry *entry_new(uint32_t type) {
    Entry *ent = new Entry();
    ent->type = type;
    switch (type) {
        case T_STR: ent->str = new std::string(); break;
        case T_ZSET: ent->zset = new ZSet(); zset_init(ent->zset); break;
        case T_LIST: ent->list = new std::deque<std::string>(); break;
        case T_HASH: ent->hash = new std::unordered_map<std::string, std::string>(); break;
        case T_SET: ent->set = new std::unordered_map<std::string, char>(); break;
    }
    return ent;
}

static void entry_set_ttl(Entry *ent, int64_t ttl_ms);

static void entry_del_sync(Entry *ent) {
    switch (ent->type) {
        case T_STR: delete ent->str; break;
        case T_ZSET: zset_clear(ent->zset); delete ent->zset; break;
        case T_LIST: delete ent->list; break;
        case T_HASH: delete ent->hash; break;
        case T_SET: delete ent->set; break;
    }
    delete ent;
}

static void entry_del_func(void *arg) { entry_del_sync((Entry *)arg); }

static size_t entry_size(Entry *ent) {
    switch (ent->type) {
        case T_ZSET: return ent->zset->sl ? ent->zset->sl->length : 0;
        case T_LIST: return ent->list->size();
        case T_HASH: return ent->hash->size();
        case T_SET: return ent->set->size();
        default: return 0;
    }
}

static void entry_del(Entry *ent) {
    entry_set_ttl(ent, -1);
    const size_t k_large_container_size = 1000;
    if (entry_size(ent) > k_large_container_size) {
        thread_pool_queue(&g_data.thread_pool, &entry_del_func, ent);
    } else {
        entry_del_sync(ent);
    }
}

struct LookupKey {
    struct HNode node;
    std::string key;
};

static bool entry_eq(HNode *node, HNode *key) {
    struct Entry *ent = container_of(node, struct Entry, node);
    struct LookupKey *keydata = container_of(key, struct LookupKey, node);
    return ent->key == keydata->key;
}

static Entry *lookup_entry(const std::string &key) {
    LookupKey lk;
    lk.key = key;
    lk.node.hcode = str_hash((uint8_t *)lk.key.data(), lk.key.size());
    HNode *node = hm_lookup(&g_data.db, &lk.node, &entry_eq);
    return node ? container_of(node, Entry, node) : NULL;
}

// Finds-or-creates `key` as `type`. Returns NULL with a WRONGTYPE error
// already written if it exists as a different type.
static Entry *get_or_create(const std::string &key, uint32_t type, Buffer &out, bool *created) {
    Entry *ent = lookup_entry(key);
    if (ent) {
        if (ent->type != type) {
            out_err(out, ERR_BAD_TYP, std::string("WRONGTYPE key holds a ") + type_name(ent->type));
            return NULL;
        }
        *created = false;
        return ent;
    }
    ent = entry_new(type);
    ent->key = key;
    ent->node.hcode = str_hash((uint8_t *)key.data(), key.size());
    hm_insert(&g_data.db, &ent->node);
    *created = true;
    return ent;
}

// ---- string -----------------------------------------------------------

static void do_get(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != T_STR) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a string");
    return out_str(out, *ent->str);
}

static void do_set(std::vector<std::string> &cmd, Buffer &out) {
    bool created;
    Entry *ent = get_or_create(cmd[1], T_STR, out, &created);
    if (!ent) return;
    *ent->str = cmd[2];
    return out_nil(out);
}

static void do_del(std::vector<std::string> &cmd, Buffer &out) {
    LookupKey key;
    key.key = cmd[1];
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);
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
    if (ttl_ms < 0 && ent->heap_idx != (size_t)-1) {
        heap_delete(g_data.heap, ent->heap_idx);
        ent->heap_idx = -1;
    } else if (ttl_ms >= 0) {
        uint64_t expire_at = get_monotonic_msec() + (uint64_t)ttl_ms;
        HeapItem item = {expire_at, &ent->heap_idx};
        heap_upsert(g_data.heap, ent->heap_idx, item);
    }
}

static bool str2int(const std::string &s, int64_t &out) {
    if (s.empty()) return false;
    char *endp = NULL;
    out = strtoll(s.c_str(), &endp, 10);
    return endp == s.c_str() + s.size();
}

static bool str2dbl(const std::string &s, double &out) {
    if (s.empty()) return false;
    char *endp = NULL;
    out = strtod(s.c_str(), &endp);
    return endp == s.c_str() + s.size() && !isnan(out);
}

static void do_expire(std::vector<std::string> &cmd, Buffer &out) {
    int64_t ttl_ms = 0;
    if (!str2int(cmd[2], ttl_ms)) return out_err(out, ERR_BAD_ARG, "expect int64");
    Entry *ent = lookup_entry(cmd[1]);
    if (ent) entry_set_ttl(ent, ttl_ms);
    return out_int(out, ent ? 1 : 0);
}

static void do_ttl(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, -2);
    if (ent->heap_idx == (size_t)-1) return out_int(out, -1);
    uint64_t expire_at = g_data.heap[ent->heap_idx].val;
    uint64_t now_ms = get_monotonic_msec();
    return out_int(out, expire_at > now_ms ? (expire_at - now_ms) : 0);
}

static bool cb_keys(HNode *node, void *arg) {
    Buffer &out = *(Buffer *)arg;
    const std::string &key = container_of(node, Entry, node)->key;
    out_str(out, key);
    return true;
}

static void do_keys(std::vector<std::string> &, Buffer &out) {
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    hm_foreach(&g_data.db, &cb_keys, (void *)&out);
}

// ---- list ---------------------------------------------------------

// LPUSH key v1 v2 v3  ->  each value is pushed to the head in turn, so the
// final list is [v3, v2, v1, ...old head], matching real Redis semantics.
static void do_lpush(std::vector<std::string> &cmd, Buffer &out) {
    bool created;
    Entry *ent = get_or_create(cmd[1], T_LIST, out, &created);
    if (!ent) return;
    for (size_t i = 2; i < cmd.size(); i++) ent->list->push_front(cmd[i]);
    return out_int(out, (int64_t)ent->list->size());
}

static void do_rpush(std::vector<std::string> &cmd, Buffer &out) {
    bool created;
    Entry *ent = get_or_create(cmd[1], T_LIST, out, &created);
    if (!ent) return;
    for (size_t i = 2; i < cmd.size(); i++) ent->list->push_back(cmd[i]);
    return out_int(out, (int64_t)ent->list->size());
}

static void do_lpop(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != T_LIST) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a list");
    if (ent->list->empty()) return out_nil(out);
    std::string v = ent->list->front();
    ent->list->pop_front();
    return out_str(out, v);
}

static void do_rpop(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != T_LIST) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a list");
    if (ent->list->empty()) return out_nil(out);
    std::string v = ent->list->back();
    ent->list->pop_back();
    return out_str(out, v);
}

static void do_llen(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != T_LIST) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a list");
    return out_int(out, (int64_t)ent->list->size());
}

// LRANGE key start stop — inclusive, negative indices count from the end,
// same as real Redis.
static void do_lrange(std::vector<std::string> &cmd, Buffer &out) {
    int64_t start = 0, stop = 0;
    if (!str2int(cmd[2], start) || !str2int(cmd[3], stop)) {
        return out_err(out, ERR_BAD_ARG, "expect int");
    }
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_arr(out, 0);
    if (ent->type != T_LIST) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a list");

    int64_t n = (int64_t)ent->list->size();
    if (start < 0) start = std::max<int64_t>(n + start, 0);
    if (stop < 0) stop = n + stop;
    if (stop >= n) stop = n - 1;
    if (start > stop || n == 0) return out_arr(out, 0);

    out_arr(out, (uint32_t)(stop - start + 1));
    for (int64_t i = start; i <= stop; i++) out_str(out, (*ent->list)[i]);
}

// ---- hash ---------------------------------------------------------

// HSET key f1 v1 f2 v2 ... — returns the number of *new* fields set.
static void do_hset(std::vector<std::string> &cmd, Buffer &out) {
    if ((cmd.size() - 2) % 2 != 0) return out_err(out, ERR_BAD_ARG, "wrong number of arguments");
    bool created;
    Entry *ent = get_or_create(cmd[1], T_HASH, out, &created);
    if (!ent) return;
    int64_t added = 0;
    for (size_t i = 2; i + 1 < cmd.size(); i += 2) {
        added += (*ent->hash).count(cmd[i]) == 0;
        (*ent->hash)[cmd[i]] = cmd[i + 1];
    }
    return out_int(out, added);
}

static void do_hget(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_nil(out);
    if (ent->type != T_HASH) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a hash");
    auto it = ent->hash->find(cmd[2]);
    if (it == ent->hash->end()) return out_nil(out);
    return out_str(out, it->second);
}

static void do_hdel(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != T_HASH) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a hash");
    int64_t removed = 0;
    for (size_t i = 2; i < cmd.size(); i++) removed += ent->hash->erase(cmd[i]);
    return out_int(out, removed);
}

static void do_hgetall(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_arr(out, 0);
    if (ent->type != T_HASH) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a hash");
    out_arr(out, (uint32_t)ent->hash->size() * 2);
    for (auto &kv : *ent->hash) {
        out_str(out, kv.first);
        out_str(out, kv.second);
    }
}

static void do_hlen(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != T_HASH) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a hash");
    return out_int(out, (int64_t)ent->hash->size());
}

// ---- set ------------------------------------------------------------

static void do_sadd(std::vector<std::string> &cmd, Buffer &out) {
    bool created;
    Entry *ent = get_or_create(cmd[1], T_SET, out, &created);
    if (!ent) return;
    int64_t added = 0;
    for (size_t i = 2; i < cmd.size(); i++) {
        added += ent->set->emplace(cmd[i], 1).second;
    }
    return out_int(out, added);
}

static void do_srem(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != T_SET) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a set");
    int64_t removed = 0;
    for (size_t i = 2; i < cmd.size(); i++) removed += ent->set->erase(cmd[i]);
    return out_int(out, removed);
}

static void do_sismember(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != T_SET) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a set");
    return out_int(out, ent->set->count(cmd[2]) ? 1 : 0);
}

static void do_smembers(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_arr(out, 0);
    if (ent->type != T_SET) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a set");
    out_arr(out, (uint32_t)ent->set->size());
    for (auto &kv : *ent->set) out_str(out, kv.first);
}

static void do_scard(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_int(out, 0);
    if (ent->type != T_SET) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a set");
    return out_int(out, (int64_t)ent->set->size());
}

// ---- sorted set (skiplist-backed) --------------------------------

static void do_zadd(std::vector<std::string> &cmd, Buffer &out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) return out_err(out, ERR_BAD_ARG, "expect float");
    bool created;
    Entry *ent = get_or_create(cmd[1], T_ZSET, out, &created);
    if (!ent) return;
    bool added = zset_insert(ent->zset, cmd[3].data(), cmd[3].size(), score);
    return out_int(out, (int64_t)added);
}

static const ZSet k_empty_zset{};

static ZSet *expect_zset(std::string &s, bool *type_err) {
    *type_err = false;
    Entry *ent = lookup_entry(s);
    if (!ent) return (ZSet *)&k_empty_zset;
    if (ent->type != T_ZSET) {
        *type_err = true;
        return NULL;
    }
    return ent->zset;
}

static void do_zrem(std::vector<std::string> &cmd, Buffer &out) {
    bool type_err;
    ZSet *zset = expect_zset(cmd[1], &type_err);
    if (type_err) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a zset");
    ZMember *m = zset_lookup(zset, cmd[2].data(), cmd[2].size());
    if (m) zset_delete(zset, m);
    return out_int(out, m ? 1 : 0);
}

static void do_zscore(std::vector<std::string> &cmd, Buffer &out) {
    bool type_err;
    ZSet *zset = expect_zset(cmd[1], &type_err);
    if (type_err) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a zset");
    ZMember *m = zset_lookup(zset, cmd[2].data(), cmd[2].size());
    return m ? out_dbl(out, m->score) : out_nil(out);
}

static void do_zquery(std::vector<std::string> &cmd, Buffer &out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) return out_err(out, ERR_BAD_ARG, "expect fp number");
    const std::string &name = cmd[3];
    int64_t offset = 0, limit = 0;
    if (!str2int(cmd[4], offset) || !str2int(cmd[5], limit)) {
        return out_err(out, ERR_BAD_ARG, "expect int");
    }
    bool type_err;
    ZSet *zset = expect_zset(cmd[1], &type_err);
    if (type_err) return out_err(out, ERR_BAD_TYP, "WRONGTYPE not a zset");
    if (limit <= 0) return out_arr(out, 0);

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
    out_end_arr(out, ctx, (uint32_t)n);
}

// ---- misc -----------------------------------------------------------

static void do_stats(std::vector<std::string> &, Buffer &out) {
    uint64_t uptime_ms = get_monotonic_msec() - g_data.start_time_ms;
    long rss_kb = get_rss_kb();

    size_t ctx = out_begin_arr(out);
    uint32_t n = 0;
    auto pair = [&](const char *k, int64_t v) {
        out_str(out, k, strlen(k));
        out_int(out, v);
        n += 2;
    };
    pair("connections", (int64_t)g_data.active_conns);
    pair("total_connections", (int64_t)g_data.total_conns);
    pair("ops_per_sec", (int64_t)g_data.ops_per_sec);
    pair("total_ops", (int64_t)g_data.total_cmds);
    pair("keys", (int64_t)hm_size(&g_data.db));
    pair("uptime_ms", (int64_t)uptime_ms);
    pair("memory_kb", (int64_t)(rss_kb >= 0 ? rss_kb : 0));
    out_end_arr(out, ctx, n);
}

static void do_type(std::vector<std::string> &cmd, Buffer &out) {
    Entry *ent = lookup_entry(cmd[1]);
    if (!ent) return out_str(out, "none", 4);
    const char *t = type_name(ent->type);
    return out_str(out, t, strlen(t));
}

static void do_request(std::vector<std::string> &cmd, Buffer &out) {
    auto is = [&](const char *name) { return cmd[0] == name; };
    if (cmd.size() == 2 && is("get")) return do_get(cmd, out);
    if (cmd.size() == 3 && is("set")) return do_set(cmd, out);
    if (cmd.size() == 2 && is("del")) return do_del(cmd, out);
    if (cmd.size() == 3 && is("pexpire")) return do_expire(cmd, out);
    if (cmd.size() == 2 && is("pttl")) return do_ttl(cmd, out);
    if (cmd.size() == 1 && is("keys")) return do_keys(cmd, out);
    if (cmd.size() == 1 && is("stats")) return do_stats(cmd, out);
    if (cmd.size() == 2 && is("type")) return do_type(cmd, out);

    if (cmd.size() >= 3 && is("lpush")) return do_lpush(cmd, out);
    if (cmd.size() >= 3 && is("rpush")) return do_rpush(cmd, out);
    if (cmd.size() == 2 && is("lpop")) return do_lpop(cmd, out);
    if (cmd.size() == 2 && is("rpop")) return do_rpop(cmd, out);
    if (cmd.size() == 2 && is("llen")) return do_llen(cmd, out);
    if (cmd.size() == 4 && is("lrange")) return do_lrange(cmd, out);

    if (cmd.size() >= 4 && is("hset")) return do_hset(cmd, out);
    if (cmd.size() == 3 && is("hget")) return do_hget(cmd, out);
    if (cmd.size() >= 3 && is("hdel")) return do_hdel(cmd, out);
    if (cmd.size() == 2 && is("hgetall")) return do_hgetall(cmd, out);
    if (cmd.size() == 2 && is("hlen")) return do_hlen(cmd, out);

    if (cmd.size() >= 3 && is("sadd")) return do_sadd(cmd, out);
    if (cmd.size() >= 3 && is("srem")) return do_srem(cmd, out);
    if (cmd.size() == 3 && is("sismember")) return do_sismember(cmd, out);
    if (cmd.size() == 2 && is("smembers")) return do_smembers(cmd, out);
    if (cmd.size() == 2 && is("scard")) return do_scard(cmd, out);

    if (cmd.size() == 4 && is("zadd")) return do_zadd(cmd, out);
    if (cmd.size() == 3 && is("zrem")) return do_zrem(cmd, out);
    if (cmd.size() == 3 && is("zscore")) return do_zscore(cmd, out);
    if (cmd.size() == 6 && is("zquery")) return do_zquery(cmd, out);

    return out_err(out, ERR_UNKNOWN, "unknown command.");
}

static void response_begin(Buffer &out, size_t *header) {
    *header = out.size();
    buf_append_u32(out, 0);
}

static size_t response_size(Buffer &out, size_t header) { return out.size() - header - 4; }

static void response_end(Buffer &out, size_t header) {
    size_t msg_size = response_size(out, header);
    if (msg_size > k_max_msg) {
        out.resize(header + 4);
        out_err(out, ERR_TOO_BIG, "response is too big.");
        msg_size = response_size(out, header);
    }
    uint32_t len = (uint32_t)msg_size;
    memcpy(&out[header], &len, 4);
}

static bool try_one_request(Conn *conn) {
    if (conn->incoming.size() < 4) return false;
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) {
        conn->want_close = true;
        return false;
    }
    if (4 + len > conn->incoming.size()) return false;
    const uint8_t *request = &conn->incoming[4];

    std::vector<std::string> cmd;
    if (parse_req(request, len, cmd) < 0) {
        msg("bad request");
        conn->want_close = true;
        return false;
    }
    size_t header_pos = 0;
    response_begin(conn->outgoing, &header_pos);
    if (cmd.empty()) {
        out_err(conn->outgoing, ERR_UNKNOWN, "empty command");
    } else {
        do_request(cmd, conn->outgoing);
    }
    response_end(conn->outgoing, header_pos);
    g_data.total_cmds++;
    g_data.cmds_this_second++;
    buf_consume(conn->incoming, 4 + len);
    return true;
}

// Edge-triggered: drain the socket until EAGAIN, not just once per event.
static void handle_write(Conn *conn) {
    while (!conn->outgoing.empty()) {
        ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());
        if (rv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            msg_errno("write() error");
            conn->want_close = true;
            return;
        }
        buf_consume(conn->outgoing, (size_t)rv);
    }
    if (conn->outgoing.empty()) {
        conn->want_read = true;
        conn->want_write = false;
    } else {
        conn->want_write = true;
    }
    conn_update_epoll(conn);
}

static void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024];
    while (true) {
        ssize_t rv = read(conn->fd, buf, sizeof(buf));
        if (rv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            msg_errno("read() error");
            conn->want_close = true;
            return;
        }
        if (rv == 0) {
            if (!conn->incoming.empty()) msg("unexpected EOF");
            conn->want_close = true;
            break;
        }
        buf_append(conn->incoming, buf, (size_t)rv);
        if ((size_t)rv < sizeof(buf)) break; // likely drained this round
    }

    while (try_one_request(conn)) {}

    if (!conn->outgoing.empty()) {
        conn->want_read = false;
        conn->want_write = true;
        handle_write(conn);
        return;
    }
    conn_update_epoll(conn);
}

const uint64_t k_idle_timeout_ms = 5 * 1000;

static int32_t next_timer_ms() {
    uint64_t now_ms = get_monotonic_msec();
    uint64_t next_ms = (uint64_t)-1;
    if (!dlist_empty(&g_data.idle_list)) {
        Conn *conn = container_of(g_data.idle_list.next, Conn, idle_node);
        next_ms = conn->last_active_ms + k_idle_timeout_ms;
    }
    if (!g_data.heap.empty() && g_data.heap[0].val < next_ms) {
        next_ms = g_data.heap[0].val;
    }
    uint64_t stats_due = g_data.last_stats_ms + 1000;
    if (stats_due < next_ms) next_ms = stats_due;
    if (next_ms == (uint64_t)-1) return -1;
    if (next_ms <= now_ms) return 0;
    return (int32_t)(next_ms - now_ms);
}

static bool hnode_same(HNode *node, HNode *key) { return node == key; }

static void process_timers() {
    uint64_t now_ms = get_monotonic_msec();
    if (now_ms - g_data.last_stats_ms >= 1000) {
        g_data.ops_per_sec = g_data.cmds_this_second;
        g_data.cmds_this_second = 0;
        g_data.last_stats_ms = now_ms;
    }
    while (!dlist_empty(&g_data.idle_list)) {
        Conn *conn = container_of(g_data.idle_list.next, Conn, idle_node);
        uint64_t next_ms = conn->last_active_ms + k_idle_timeout_ms;
        if (next_ms >= now_ms) break;
        fprintf(stderr, "removing idle connection: %d\n", conn->fd);
        conn_destroy(conn);
    }
    const size_t k_max_works = 2000;
    size_t nworks = 0;
    const std::vector<HeapItem> &heap = g_data.heap;
    while (!heap.empty() && heap[0].val < now_ms) {
        Entry *ent = container_of(heap[0].ref, Entry, heap_idx);
        HNode *node = hm_delete(&g_data.db, &ent->node, &hnode_same);
        assert(node == &ent->node);
        entry_del(ent);
        if (nworks++ >= k_max_works) break;
    }
}

int main() {
    dlist_init(&g_data.idle_list);
    thread_pool_init(&g_data.thread_pool, 4);
    g_data.start_time_ms = get_monotonic_msec();
    g_data.last_stats_ms = g_data.start_time_ms;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) die("bind()");

    fd_set_nb(fd);
    rv = listen(fd, SOMAXCONN);
    if (rv) die("listen()");

    g_data.epfd = epoll_create1(0);
    if (g_data.epfd < 0) die("epoll_create1()");

    struct epoll_event listen_ev = {};
    listen_ev.events = EPOLLIN; // level-triggered on the listener: keep
                                 // accepting until accept() returns EAGAIN.
    listen_ev.data.fd = fd;
    if (epoll_ctl(g_data.epfd, EPOLL_CTL_ADD, fd, &listen_ev) < 0) die("epoll_ctl(listen)");

    const int k_max_events = 256;
    std::vector<struct epoll_event> events(k_max_events);

    while (true) {
        int32_t timeout_ms = next_timer_ms();
        int n = epoll_wait(g_data.epfd, events.data(), k_max_events, timeout_ms);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) die("epoll_wait");

        for (int i = 0; i < n; i++) {
            int evfd = events[i].data.fd;
            uint32_t flags = events[i].events;

            if (evfd == fd) {
                // Listener is level-triggered and non-blocking: drain the
                // accept backlog on every wakeup.
                while (handle_accept(fd) == 0) {}
                continue;
            }

            Conn *conn = g_data.fd2conn[evfd];
            if (!conn) continue; // already destroyed earlier this batch

            conn->last_active_ms = get_monotonic_msec();
            dlist_detach(&conn->idle_node);
            dlist_insert_before(&g_data.idle_list, &conn->idle_node);

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

#elif defined(BUILD_CLIENT)

static void msg(const char *msg) { fprintf(stderr, "%s\n", msg); }
static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) return -1;
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) return -1;
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

const size_t k_max_msg = 4096;

static int32_t send_req(int fd, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const std::string &s : cmd) len += 4 + s.size();
    if (len > k_max_msg) return -1;

    char wbuf[4 + k_max_msg];
    memcpy(&wbuf[0], &len, 4);
    uint32_t n = (uint32_t)cmd.size();
    memcpy(&wbuf[4], &n, 4);
    size_t cur = 8;
    for (const std::string &s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }
    return write_all(fd, wbuf, 4 + len);
}

enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

static int32_t print_response(const uint8_t *data, size_t size) {
    if (size < 1) { msg("bad response"); return -1; }
    switch (data[0]) {
    case TAG_NIL:
        printf("(nil)\n");
        return 1;
    case TAG_ERR:
        if (size < 1 + 8) { msg("bad response"); return -1; }
        {
            int32_t code = 0;
            uint32_t len = 0;
            memcpy(&code, &data[1], 4);
            memcpy(&len, &data[1 + 4], 4);
            if (size < 1 + 8 + len) { msg("bad response"); return -1; }
            printf("(err) %d %.*s\n", code, len, &data[1 + 8]);
            return 1 + 8 + len;
        }
    case TAG_STR:
        if (size < 1 + 4) { msg("bad response"); return -1; }
        {
            uint32_t len = 0;
            memcpy(&len, &data[1], 4);
            if (size < 1 + 4 + len) { msg("bad response"); return -1; }
            printf("(str) %.*s\n", len, &data[1 + 4]);
            return 1 + 4 + len;
        }
    case TAG_INT:
        if (size < 1 + 8) { msg("bad response"); return -1; }
        {
            int64_t val = 0;
            memcpy(&val, &data[1], 8);
            printf("(int) %ld\n", val);
            return 1 + 8;
        }
    case TAG_DBL:
        if (size < 1 + 8) { msg("bad response"); return -1; }
        {
            double val = 0;
            memcpy(&val, &data[1], 8);
            printf("(dbl) %g\n", val);
            return 1 + 8;
        }
    case TAG_ARR:
        if (size < 1 + 4) { msg("bad response"); return -1; }
        {
            uint32_t len = 0;
            memcpy(&len, &data[1], 4);
            printf("(arr) len=%u\n", len);
            size_t arr_bytes = 1 + 4;
            for (uint32_t i = 0; i < len; ++i) {
                int32_t rv = print_response(&data[arr_bytes], size - arr_bytes);
                if (rv < 0) return rv;
                arr_bytes += (size_t)rv;
            }
            printf("(arr) end\n");
            return (int32_t)arr_bytes;
        }
    default:
        msg("bad response");
        return -1;
    }
}

static int32_t read_res(int fd) {
    char rbuf[4 + k_max_msg];
    errno = 0;
    int32_t err = read_full(fd, rbuf, 4);
    if (err) {
        msg(errno == 0 ? "EOF" : "read() error");
        return err;
    }
    uint32_t len = 0;
    memcpy(&len, rbuf, 4);
    if (len > k_max_msg) { msg("too long"); return -1; }

    err = read_full(fd, &rbuf[4], len);
    if (err) { msg("read() error"); return err; }

    int32_t rv = print_response((uint8_t *)&rbuf[4], len);
    if (rv > 0 && (uint32_t)rv != len) { msg("bad response"); rv = -1; }
    return rv;
}

int main(int argc, char **argv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) die("connect");

    std::vector<std::string> cmd;
    for (int i = 1; i < argc; ++i) cmd.push_back(argv[i]);
    int32_t err = send_req(fd, cmd);
    if (!err) err = read_res(fd);

    close(fd);
    return 0;
}

#elif defined(BUILD_TEST)

#else
#error "Define BUILD_SERVER, BUILD_CLIENT, or BUILD_TEST when compiling redis.cpp"
#endif