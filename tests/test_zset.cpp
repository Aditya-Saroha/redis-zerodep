/* Unit tests for ZSet — the skiplist + hashmap pairing that backs sorted
 * sets, checking the two structures stay in agreement with each other.
 * Build: g++ -O2 -std=gnu++17 -Wall -Wextra -pthread -DBUILD_TEST \
 *            -o test_zset tests/test_zset.cpp
 */
#define BUILD_TEST
#include "../src/zset.hpp"
#include "test_common.h"

TEST(insert_new_member_returns_true_score_update_returns_false) {
    ZSet zset;
    zset_init(&zset);

    CHECK(zset_insert(&zset, "alice", 5, 10.0));
    CHECK(!zset_insert(&zset, "alice", 5, 20.0)); // same member, new score

    ZMember *m = zset_lookup(&zset, "alice", 5);
    CHECK(m != NULL);
    CHECK_EQ(m->score, 20.0);
    CHECK_EQ(zset.sl->length, (uint64_t)1); // still one member, not two

    zset_clear(&zset);
}

TEST(score_update_reorders_the_skiplist) {
    ZSet zset;
    zset_init(&zset);
    zset_insert(&zset, "low", 3, 1.0);
    zset_insert(&zset, "high", 4, 100.0);

    // "low" overtakes "high"
    zset_insert(&zset, "low", 3, 999.0);

    SkiplistNode *first = zset.sl->header->level[0].forward;
    CHECK(first != NULL);
    CHECK_EQ(first->member, std::string("high"));
    CHECK_EQ(first->level[0].forward->member, std::string("low"));

    zset_clear(&zset);
}

TEST(lookup_agrees_between_hashmap_and_skiplist) {
    ZSet zset;
    zset_init(&zset);
    const char *names[] = {"a", "b", "c", "d", "e"};
    for (int i = 0; i < 5; i++) {
        zset_insert(&zset, names[i], strlen(names[i]), (double)i);
    }
    CHECK_EQ(zset.sl->length, (uint64_t)5);
    CHECK_EQ(hm_size(&zset.hmap), (size_t)5);

    ZMember *m = zset_lookup(&zset, "c", 1);
    CHECK(m != NULL);
    CHECK_EQ(m->score, 2.0);
    CHECK(zset_lookup(&zset, "nope", 4) == NULL);

    zset_clear(&zset);
}

TEST(delete_removes_from_both_structures_and_stays_consistent) {
    ZSet zset;
    zset_init(&zset);
    zset_insert(&zset, "one", 3, 1.0);
    zset_insert(&zset, "two", 3, 2.0);
    zset_insert(&zset, "three", 5, 3.0);

    ZMember *m = zset_lookup(&zset, "two", 3);
    CHECK(m != NULL);
    zset_delete(&zset, m);

    CHECK(zset_lookup(&zset, "two", 3) == NULL);
    CHECK_EQ(zset.sl->length, (uint64_t)2);
    CHECK_EQ(hm_size(&zset.hmap), (size_t)2);

    // remaining members still walk in score order
    SkiplistNode *n = zset.sl->header->level[0].forward;
    CHECK_EQ(n->member, std::string("one"));
    n = n->level[0].forward;
    CHECK_EQ(n->member, std::string("three"));

    zset_clear(&zset);
}

TEST(seekge_matches_the_underlying_skiplist) {
    ZSet zset;
    zset_init(&zset);
    for (int i = 0; i < 10; i++) {
        std::string name = "m" + std::to_string(i);
        zset_insert(&zset, name.data(), name.size(), (double)(i * 5));
    }
    SkiplistNode *n = zset_seekge(&zset, 23.0, "", 0);
    CHECK(n != NULL);
    CHECK_EQ(n->score, 25.0); // first score >= 23 is 25 (m5)
    CHECK_EQ(n->member, std::string("m5"));

    // past the end
    CHECK(zset_seekge(&zset, 10000.0, "", 0) == NULL);

    zset_clear(&zset);
}

TEST(seekge_on_an_empty_zset_returns_null) {
    ZSet zset;
    zset_init(&zset);
    CHECK(zset_seekge(&zset, 0.0, "", 0) == NULL);
    zset_clear(&zset);
}

TEST(large_insert_then_delete_all_leaves_zset_empty) {
    ZSet zset;
    zset_init(&zset);
    const int N = 1000;
    for (int i = 0; i < N; i++) {
        std::string name = "member" + std::to_string(i);
        zset_insert(&zset, name.data(), name.size(), (double)(i % 37));
    }
    CHECK_EQ(zset.sl->length, (uint64_t)N);

    for (int i = 0; i < N; i++) {
        std::string name = "member" + std::to_string(i);
        ZMember *m = zset_lookup(&zset, name.data(), name.size());
        CHECK(m != NULL);
        zset_delete(&zset, m);
    }
    CHECK_EQ(zset.sl->length, (uint64_t)0);
    CHECK_EQ(hm_size(&zset.hmap), (size_t)0);

    zset_clear(&zset);
}

int main() {
    return run_all_tests();
}