#pragma once
#include "common.hpp"
#include <cmath>

constexpr int SKIPLIST_MAXLEVEL = 32;
constexpr double SKIPLIST_P = 0.25;

struct SkiplistNode {
    std::string member;
    double score = 0;
    SkiplistNode *backward = nullptr;
    struct Level {
        SkiplistNode *forward = nullptr;
        uint64_t span = 0;
    };
    std::vector<Level> level;
};

struct Skiplist {
    SkiplistNode *header = nullptr;
    SkiplistNode *tail = nullptr;
    int level = 1;
    uint64_t length = 0;
};

inline SkiplistNode *sl_new_node(int lvl, double score, const std::string &member) {
    SkiplistNode *n = new SkiplistNode();
    n->score = score;
    n->member = member;
    n->level.resize(lvl);
    return n;
}

inline int sl_random_level() {
    int lvl = 1;
    while (lvl < SKIPLIST_MAXLEVEL && (rand() & 0xFFFF) < (int)(SKIPLIST_P * 0xFFFF)) lvl++;
    return lvl;
}

inline Skiplist *sl_create() {
    Skiplist *sl = new Skiplist();
    sl->header = sl_new_node(SKIPLIST_MAXLEVEL, -HUGE_VAL, "");
    sl->level = 1;
    sl->length = 0;
    return sl;
}

inline void sl_free(Skiplist *sl) {
    SkiplistNode *node = sl->header->level[0].forward;
    while (node) {
        SkiplistNode *next = node->level[0].forward;
        delete node;
        node = next;
    }
    delete sl->header;
    delete sl;
}

inline bool sl_less(double s1, const std::string &m1, double s2, const std::string &m2) {
    if (s1 != s2) return s1 < s2;
    return m1 < m2;
}

inline SkiplistNode *sl_insert(Skiplist *sl, double score, const std::string &member) {
    SkiplistNode *update[SKIPLIST_MAXLEVEL];
    uint64_t rank[SKIPLIST_MAXLEVEL];
    SkiplistNode *x = sl->header;
    
    for (int i = sl->level - 1; i >= 0; i--) {
        rank[i] = (i == sl->level - 1) ? 0 : rank[i + 1];
        while (x->level[i].forward && sl_less(x->level[i].forward->score, x->level[i].forward->member, score, member)) {
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
    for (int i = lvl; i < sl->level; i++) update[i]->level[i].span++;

    node->backward = (update[0] == sl->header) ? nullptr : update[0];
    if (node->level[0].forward) node->level[0].forward->backward = node;
    else sl->tail = node;
    sl->length++;
    return node;
}

inline void sl_delete_node(Skiplist *sl, SkiplistNode *update[], SkiplistNode *x) {
    for (int i = 0; i < sl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            update[i]->level[i].span--;
        }
    }
    if (x->level[0].forward) x->level[0].forward->backward = x->backward;
    else sl->tail = x->backward;
    while (sl->level > 1 && !sl->header->level[sl->level - 1].forward) sl->level--;
    sl->length--;
}

inline bool sl_delete(Skiplist *sl, double score, const std::string &member) {
    SkiplistNode *update[SKIPLIST_MAXLEVEL];
    SkiplistNode *x = sl->header;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && sl_less(x->level[i].forward->score, x->level[i].forward->member, score, member)) {
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

inline SkiplistNode *sl_first_ge(Skiplist *sl, double score, const std::string &member) {
    SkiplistNode *x = sl->header;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && sl_less(x->level[i].forward->score, x->level[i].forward->member, score, member)) {
            x = x->level[i].forward;
        }
    }
    return x->level[0].forward;
}

inline SkiplistNode *sl_offset(SkiplistNode *node, int64_t offset) {
    while (node && offset > 0) { node = node->level[0].forward; offset--; }
    while (node && offset < 0) { node = node->backward; offset++; }
    return node;
}