#pragma once
#include "common.hpp"
#include "hmap.hpp"
#include "skiplist.hpp"

struct ZSet {
    Skiplist *sl = nullptr;
    HMap hmap;
};

struct ZMember {
    HNode hmap;
    std::string name;
    double score = 0;
};

inline void zset_init(ZSet *zset) {
    zset->sl = sl_create();
    zset->hmap = HMap{};
}

struct ZKey {
    HNode node;
    const char *name = nullptr;
    size_t len = 0;
};

inline bool zmember_eq(HNode *node, HNode *key) {
    ZMember *m = container_of(node, ZMember, hmap);
    ZKey *k = container_of(key, ZKey, node);
    if (m->name.size() != k->len) return false;
    return memcmp(m->name.data(), k->name, k->len) == 0;
}

inline ZMember *zset_lookup(ZSet *zset, const char *name, size_t len) {
    if (!zset->sl) return nullptr;
    ZKey key;
    key.node.hcode = str_hash(reinterpret_cast<const uint8_t*>(name), len);
    key.name = name;
    key.len = len;
    HNode *found = hm_lookup(&zset->hmap, &key.node, &zmember_eq);
    return found ? container_of(found, ZMember, hmap) : nullptr;
}

inline bool zset_insert(ZSet *zset, const char *name, size_t len, double score) {
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
    m->hmap.hcode = str_hash(reinterpret_cast<const uint8_t*>(name), len);
    hm_insert(&zset->hmap, &m->hmap);
    return true;
}

inline void zset_delete(ZSet *zset, ZMember *m) {
    ZKey key;
    key.node.hcode = m->hmap.hcode;
    key.name = m->name.data();
    key.len = m->name.size();
    HNode *found = hm_delete(&zset->hmap, &key.node, &zmember_eq);
    assert(found);
    sl_delete(zset->sl, m->score, m->name);
    delete container_of(found, ZMember, hmap);
}

inline SkiplistNode *zset_seekge(ZSet *zset, double score, const char *name, size_t len) {
    if (!zset->sl) return nullptr;
    return sl_first_ge(zset->sl, score, std::string(name, len));
}

inline void zset_clear(ZSet *zset) {
    struct Ctx { std::vector<ZMember *> members; };
    Ctx ctx;
    auto collect = [](HNode *node, void *arg) -> bool {
        static_cast<Ctx *>(arg)->members.push_back(container_of(node, ZMember, hmap));
        return true;
    };
    hm_foreach(&zset->hmap, collect, &ctx);
    for (ZMember *m : ctx.members) delete m;
    hm_clear(&zset->hmap);
    if (zset->sl) { sl_free(zset->sl); zset->sl = nullptr; }
}