// Unit tests for HMap (the progressive-rehash hashtable behind the
// keyspace and the zset member index).
// Build: g++ -O2 -std=gnu++17 -Wall -Wextra -pthread -DBUILD_TEST \
//            -o test_hashtable tests/test_hashtable.cpp
#define BUILD_TEST
#include "../src/redis.cpp"
#include "test_common.h"

struct StrNode {
    HNode node;
    std::string key;
};

static bool str_eq(HNode *a, HNode *b) {
    StrNode *sa = container_of(a, StrNode, node);
    StrNode *sb = container_of(b, StrNode, node);
    return sa->key == sb->key;
}

static StrNode *make_node(const std::string &key) {
    StrNode *n = new StrNode();
    n->key = key;
    n->node.hcode = str_hash((uint8_t *)key.data(), key.size());
    return n;
}

static HNode *lookup_key(HMap *hmap, const std::string &key) {
    StrNode probe;
    probe.key = key;
    probe.node.hcode = str_hash((uint8_t *)key.data(), key.size());
    return hm_lookup(hmap, &probe.node, &str_eq);
}

TEST(insert_and_lookup_roundtrip) {
    HMap hmap{};
    StrNode *a = make_node("alpha");
    hm_insert(&hmap, &a->node);
    CHECK_EQ(hm_size(&hmap), (size_t)1);

    HNode *found = lookup_key(&hmap, "alpha");
    CHECK(found != NULL);
    CHECK_EQ(container_of(found, StrNode, node)->key, std::string("alpha"));
    CHECK(lookup_key(&hmap, "missing") == NULL);

    hm_clear(&hmap);
    delete a;
}

TEST(lookup_on_empty_map_returns_null_not_a_crash) {
    HMap hmap{};
    CHECK(lookup_key(&hmap, "anything") == NULL);
    hm_clear(&hmap);
}

TEST(delete_removes_entry_and_frees_the_slot) {
    HMap hmap{};
    StrNode *a = make_node("k");
    hm_insert(&hmap, &a->node);

    HNode *removed = hm_delete(&hmap, &a->node, &str_eq);
    CHECK(removed == &a->node);
    CHECK_EQ(hm_size(&hmap), (size_t)0);
    CHECK(lookup_key(&hmap, "k") == NULL);
    // deleting again finds nothing
    CHECK(hm_delete(&hmap, &a->node, &str_eq) == NULL);

    hm_clear(&hmap);
    delete a;
}

TEST(collisions_in_the_same_bucket_are_both_findable) {
    // Two distinct keys, forced into the same bucket by giving them the
    // same hash code — exercises the chaining path, not just the common
    // case where every key lands in its own slot.
    HMap hmap{};
    StrNode *a = make_node("one");
    StrNode *b = make_node("two");
    b->node.hcode = a->node.hcode; // force a collision
    hm_insert(&hmap, &a->node);
    hm_insert(&hmap, &b->node);

    StrNode probe_a; probe_a.key = "one"; probe_a.node.hcode = a->node.hcode;
    StrNode probe_b; probe_b.key = "two"; probe_b.node.hcode = b->node.hcode;
    HNode *fa = hm_lookup(&hmap, &probe_a.node, &str_eq);
    HNode *fb = hm_lookup(&hmap, &probe_b.node, &str_eq);
    CHECK(fa == &a->node);
    CHECK(fb == &b->node);

    hm_clear(&hmap);
    delete a;
    delete b;
}

TEST(incremental_rehash_preserves_every_entry_under_load) {
    HMap hmap{};
    const int N = 5000; // well past the load-factor threshold: forces a rehash
    std::vector<StrNode *> nodes;
    for (int i = 0; i < N; i++) {
        StrNode *n = make_node("key" + std::to_string(i));
        nodes.push_back(n);
        hm_insert(&hmap, &n->node);
    }
    CHECK_EQ(hm_size(&hmap), (size_t)N);

    // every key must still resolve correctly while migration is (or was)
    // in flight -- this is the property incremental rehashing exists for
    for (int i = 0; i < N; i++) {
        CHECK(lookup_key(&hmap, "key" + std::to_string(i)) != NULL);
    }

    // enough lookups (each one calls hm_help_rehashing) should have fully
    // drained the older generation by now
    CHECK(hmap.older.tab == NULL);

    hm_clear(&hmap);
    for (StrNode *n : nodes) delete n;
}

TEST(delete_mid_rehash_finds_entries_regardless_of_generation) {
    HMap hmap{};
    std::vector<StrNode *> nodes;
    for (int i = 0; i < 200; i++) {
        StrNode *n = make_node("x" + std::to_string(i));
        nodes.push_back(n);
        hm_insert(&hmap, &n->node);
    }
    // delete half immediately -- a rehash may still be in progress, so
    // this exercises hm_delete's fall-through from newer to older table
    int deleted = 0;
    for (int i = 0; i < 100; i++) {
        if (hm_delete(&hmap, &nodes[i]->node, &str_eq)) deleted++;
    }
    CHECK_EQ(deleted, 100);
    CHECK_EQ(hm_size(&hmap), (size_t)100);
    for (int i = 100; i < 200; i++) {
        CHECK(lookup_key(&hmap, "x" + std::to_string(i)) != NULL);
    }

    hm_clear(&hmap);
    for (StrNode *n : nodes) delete n;
}

int main() {
    return run_all_tests();
}