// Unit tests for the skiplist that backs sorted sets.
// Build: g++ -O2 -std=gnu++17 -Wall -Wextra -pthread -DBUILD_TEST \
//            -o test_skiplist tests/test_skiplist.cpp
#define BUILD_TEST
#include "../src/redis.cpp"
#include "test_common.h"

TEST(insert_keeps_score_order) {
    Skiplist *sl = sl_create();
    sl_insert(sl, 3.0, "c");
    sl_insert(sl, 1.0, "a");
    sl_insert(sl, 2.0, "b");
    CHECK_EQ(sl->length, (uint64_t)3);

    SkiplistNode *n = sl->header->level[0].forward;
    CHECK(n != NULL);
    CHECK_EQ(n->member, std::string("a"));
    n = n->level[0].forward;
    CHECK(n != NULL);
    CHECK_EQ(n->member, std::string("b"));
    n = n->level[0].forward;
    CHECK(n != NULL);
    CHECK_EQ(n->member, std::string("c"));
    CHECK(n->level[0].forward == NULL);
    sl_free(sl);
}

TEST(equal_scores_break_ties_by_member_name) {
    Skiplist *sl = sl_create();
    sl_insert(sl, 5.0, "zebra");
    sl_insert(sl, 5.0, "apple");
    sl_insert(sl, 5.0, "mango");
    CHECK_EQ(sl->length, (uint64_t)3);

    SkiplistNode *n = sl->header->level[0].forward;
    CHECK_EQ(n->member, std::string("apple"));
    n = n->level[0].forward;
    CHECK_EQ(n->member, std::string("mango"));
    n = n->level[0].forward;
    CHECK_EQ(n->member, std::string("zebra"));
    sl_free(sl);
}

TEST(delete_missing_member_is_a_no_op) {
    Skiplist *sl = sl_create();
    sl_insert(sl, 1.0, "a");
    CHECK(!sl_delete(sl, 99.0, "nope"));
    CHECK(!sl_delete(sl, 1.0, "wrong-name-same-score"));
    CHECK_EQ(sl->length, (uint64_t)1);
    sl_free(sl);
}

TEST(delete_removes_node_and_keeps_remaining_order) {
    Skiplist *sl = sl_create();
    for (int i = 0; i < 20; i++) {
        sl_insert(sl, (double)i, "m" + std::to_string(i));
    }
    CHECK_EQ(sl->length, (uint64_t)20);

    CHECK(sl_delete(sl, 10.0, "m10"));
    CHECK_EQ(sl->length, (uint64_t)19);
    // deleting the same member twice fails the second time
    CHECK(!sl_delete(sl, 10.0, "m10"));

    double prev = -HUGE_VAL;
    int count = 0;
    for (SkiplistNode *n = sl->header->level[0].forward; n; n = n->level[0].forward) {
        CHECK(n->score > prev);
        CHECK(n->member != std::string("m10"));
        prev = n->score;
        count++;
    }
    CHECK_EQ(count, 19);
    sl_free(sl);
}

TEST(delete_first_and_last_node_updates_tail_and_backward_links) {
    Skiplist *sl = sl_create();
    sl_insert(sl, 1.0, "first");
    sl_insert(sl, 2.0, "mid");
    sl_insert(sl, 3.0, "last");

    CHECK(sl_delete(sl, 3.0, "last"));
    CHECK(sl->tail != NULL);
    CHECK_EQ(sl->tail->member, std::string("mid"));

    CHECK(sl_delete(sl, 1.0, "first"));
    SkiplistNode *only = sl->header->level[0].forward;
    CHECK(only != NULL);
    CHECK_EQ(only->member, std::string("mid"));
    CHECK(only->backward == NULL);
    sl_free(sl);
}

TEST(first_ge_finds_exact_and_nearest_and_past_end) {
    Skiplist *sl = sl_create();
    for (int i = 0; i < 10; i++) {
        sl_insert(sl, (double)(i * 10), "m" + std::to_string(i));
    }

    // exact (score, member) match
    SkiplistNode *n = sl_first_ge(sl, 50.0, "m5");
    CHECK(n != NULL);
    CHECK_EQ(n->member, std::string("m5"));

    // score falls between two entries -> lands on the next one up
    n = sl_first_ge(sl, 55.0, "");
    CHECK(n != NULL);
    CHECK_EQ(n->score, 60.0);

    // before the first entry -> first entry
    n = sl_first_ge(sl, -100.0, "");
    CHECK(n != NULL);
    CHECK_EQ(n->score, 0.0);

    // past the last entry -> NULL, not a crash
    n = sl_first_ge(sl, 1000.0, "");
    CHECK(n == NULL);
    sl_free(sl);
}

TEST(offset_walks_forward_and_backward_and_stops_at_ends) {
    Skiplist *sl = sl_create();
    for (int i = 0; i < 10; i++) {
        sl_insert(sl, (double)i, "m" + std::to_string(i));
    }
    SkiplistNode *first = sl->header->level[0].forward; // m0

    SkiplistNode *fwd3 = sl_offset(first, 3);
    CHECK(fwd3 != NULL);
    CHECK_EQ(fwd3->member, std::string("m3"));

    SkiplistNode *back_to_start = sl_offset(fwd3, -3);
    CHECK(back_to_start == first);

    // walking off either end returns NULL rather than dereferencing garbage
    CHECK(sl_offset(first, -1) == NULL);
    CHECK(sl_offset(first, 100) == NULL);

    // offset 0 is a no-op
    CHECK(sl_offset(fwd3, 0) == fwd3);
    sl_free(sl);
}

TEST(large_random_insert_stays_sorted_with_correct_length) {
    Skiplist *sl = sl_create();
    const int N = 2000;
    srand(42); // fixed seed: deterministic, reproducible failure if this breaks
    for (int i = 0; i < N; i++) {
        double score = (double)(rand() % 1000);
        sl_insert(sl, score, "key" + std::to_string(i));
    }
    CHECK_EQ(sl->length, (uint64_t)N);

    double prev_score = -HUGE_VAL;
    std::string prev_member;
    int count = 0;
    for (SkiplistNode *n = sl->header->level[0].forward; n; n = n->level[0].forward) {
        bool ordered = (n->score > prev_score) ||
                        (n->score == prev_score && n->member > prev_member);
        CHECK(ordered);
        prev_score = n->score;
        prev_member = n->member;
        count++;
    }
    CHECK_EQ(count, N);
    sl_free(sl);
}

TEST(deleting_every_element_leaves_an_empty_list) {
    Skiplist *sl = sl_create();
    std::vector<std::pair<double, std::string>> inserted;
    for (int i = 0; i < 100; i++) {
        std::string m = "m" + std::to_string(i);
        sl_insert(sl, (double)(i % 7), m); // lots of tied scores on purpose
        inserted.push_back({(double)(i % 7), m});
    }
    for (auto &p : inserted) {
        CHECK(sl_delete(sl, p.first, p.second));
    }
    CHECK_EQ(sl->length, (uint64_t)0);
    CHECK(sl->header->level[0].forward == NULL);
    CHECK(sl->tail == NULL);
    sl_free(sl);
}

int main() {
    return run_all_tests();
}