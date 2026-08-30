#pragma once
#include "common.hpp"

struct HNode {
    HNode *next = nullptr;
    uint64_t hcode = 0;
};

struct HTab {
    HNode **tab = nullptr;
    size_t mask = 0;
    size_t size = 0;
};

struct HMap {
    HTab newer;
    HTab older;
    size_t migrate_pos = 0;
};

inline void h_init(HTab *htab, size_t n) {
    assert(n > 0 && ((n - 1) & n) == 0);
    htab->tab = new HNode*[n]();
    htab->mask = n - 1;
    htab->size = 0;
}

inline void h_insert(HTab *htab, HNode *node) {
    size_t pos = node->hcode & htab->mask;
    node->next = htab->tab[pos];
    htab->tab[pos] = node;
    htab->size++;
}

inline HNode **h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *)) {
    if (!htab->tab) return nullptr;
    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];
    for (HNode *cur; (cur = *from) != nullptr; from = &cur->next) {
        if (cur->hcode == key->hcode && eq(cur, key)) return from;
    }
    return nullptr;
}

inline HNode *h_detach(HTab *htab, HNode **from) {
    HNode *node = *from;
    *from = node->next;
    htab->size--;
    return node;
}

constexpr size_t k_rehashing_work = 128;
constexpr size_t k_max_load_factor = 8;

inline void hm_help_rehashing(HMap *hmap) {
    size_t nwork = 0;
    while (nwork < k_rehashing_work && hmap->older.size > 0) {
        HNode **from = &hmap->older.tab[hmap->migrate_pos];
        if (!*from) { hmap->migrate_pos++; continue; }
        h_insert(&hmap->newer, h_detach(&hmap->older, from));
        nwork++;
    }
    if (hmap->older.size == 0 && hmap->older.tab) {
        delete[] hmap->older.tab;
        hmap->older = HTab{};
    }
}

inline void hm_trigger_rehashing(HMap *hmap) {
    assert(hmap->older.tab == nullptr);
    hmap->older = hmap->newer;
    h_init(&hmap->newer, (hmap->newer.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

inline HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);
    if (HNode **from = h_lookup(&hmap->newer, key, eq)) return *from;
    if (HNode **from = h_lookup(&hmap->older, key, eq)) return *from;
    return nullptr;
}

inline void hm_insert(HMap *hmap, HNode *node) {
    if (!hmap->newer.tab) h_init(&hmap->newer, 4);
    h_insert(&hmap->newer, node);
    if (!hmap->older.tab) {
        size_t threshold = (hmap->newer.mask + 1) * k_max_load_factor;
        if (hmap->newer.size >= threshold) hm_trigger_rehashing(hmap);
    }
    hm_help_rehashing(hmap);
}

inline HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);
    if (HNode **from = h_lookup(&hmap->newer, key, eq)) return h_detach(&hmap->newer, from);
    if (HNode **from = h_lookup(&hmap->older, key, eq)) return h_detach(&hmap->older, from);
    return nullptr;
}

inline void hm_clear(HMap *hmap) {
    delete[] hmap->newer.tab;
    delete[] hmap->older.tab;
    *hmap = HMap{};
}

inline size_t hm_size(HMap *hmap) { return hmap->newer.size + hmap->older.size; }

inline void hm_foreach(HMap *hmap, bool (*f)(HNode *, void *), void *arg) {
    auto h_foreach_tab = [](HTab *htab, bool (*func)(HNode *, void *), void *a) {
        if (!htab->tab) return true;
        for (size_t i = 0; i <= htab->mask; i++) {
            for (HNode *node = htab->tab[i]; node != nullptr; node = node->next) {
                if (!func(node, a)) return false;
            }
        }
        return true;
    };
    h_foreach_tab(&hmap->newer, f, arg) && h_foreach_tab(&hmap->older, f, arg);
}