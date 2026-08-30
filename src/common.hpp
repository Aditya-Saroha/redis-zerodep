#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <vector>
#include <string>
#include <deque>
#include <pthread.h>

#define container_of(ptr, type, member) ({ \
    const decltype(((type *)0)->member) *__mptr = (ptr); \
    (type *)((char *)__mptr - offsetof(type, member)); })

inline uint64_t str_hash(const uint8_t *data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

using Buffer = std::vector<uint8_t>;

struct DList {
    DList *prev = nullptr;
    DList *next = nullptr;
};

inline void dlist_init(DList *node) { node->prev = node->next = node; }
inline bool dlist_empty(DList *node) { return node->next == node; }

inline void dlist_detach(DList *node) {
    if (node->next == node && node->prev == node) return;
    DList *prev = node->prev;
    DList *next = node->next;
    prev->next = next;
    next->prev = prev;
    node->prev = node;
    node->next = node;
}

inline void dlist_insert_before(DList *target, DList *rookie) {
    DList *prev = target->prev;
    rookie->prev = prev;
    rookie->next = target;
    prev->next = rookie;
    target->prev = rookie;
}

struct HeapItem {
    uint64_t val = 0;
    size_t *ref = nullptr;
};

inline size_t heap_parent(size_t i) { return (i + 1) / 2 - 1; }
inline size_t heap_left(size_t i) { return i * 2 + 1; }
inline size_t heap_right(size_t i) { return i * 2 + 2; }

inline void heap_up(HeapItem *a, size_t pos) {
    HeapItem t = a[pos];
    while (pos > 0 && a[heap_parent(pos)].val > t.val) {
        a[pos] = a[heap_parent(pos)];
        *a[pos].ref = pos;
        pos = heap_parent(pos);
    }
    a[pos] = t;
    *a[pos].ref = pos;
}

inline void heap_down(HeapItem *a, size_t pos, size_t len) {
    HeapItem t = a[pos];
    while (true) {
        size_t l = heap_left(pos);
        size_t r = heap_right(pos);
        size_t min_pos = pos;
        uint64_t min_val = t.val;
        if (l < len && a[l].val < min_val) { min_pos = l; min_val = a[l].val; }
        if (r < len && a[r].val < min_val) { min_pos = r; }
        if (min_pos == pos) break;
        a[pos] = a[min_pos];
        *a[pos].ref = pos;
        pos = min_pos;
    }
    a[pos] = t;
    *a[pos].ref = pos;
}

inline void heap_update(HeapItem *a, size_t pos, size_t len) {
    if (pos > 0 && a[heap_parent(pos)].val > a[pos].val) {
        heap_up(a, pos);
    } else {
        heap_down(a, pos, len);
    }
}

struct Work {
    void (*f)(void *) = nullptr;
    void *arg = nullptr;
};

class ThreadPool {
public:
    std::vector<pthread_t> threads;
    std::deque<Work> queue;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;

    static void* worker(void* arg) {
        ThreadPool* tp = static_cast<ThreadPool*>(arg);
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
        return nullptr;
    }

    void init(size_t num_threads) {
        assert(num_threads > 0);
        pthread_mutex_init(&mu, nullptr);
        pthread_cond_init(&not_empty, nullptr);
        threads.resize(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            pthread_create(&threads[i], nullptr, &worker, this);
        }
    }

    void queue_work(void (*f)(void*), void* arg) {
        pthread_mutex_lock(&mu);
        queue.push_back(Work{f, arg});
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&mu);
    }
};