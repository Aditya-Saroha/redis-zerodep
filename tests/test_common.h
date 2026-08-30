// Minimal assertion-based test harness. C++ has no test framework in the
// standard library, so per the hackathon's own rule ("if your language
// ships no test framework, ... disclose it"), this is the disclosed
// substitute: plain stdlib, registered via static initialization, no
// third-party dependency to name in STDLIB.md.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase> &test_registry() {
    static std::vector<TestCase> reg;
    return reg;
}

struct TestRegistrar {
    TestRegistrar(const char *name, std::function<void()> fn) {
        test_registry().push_back({name, fn});
    }
};

#define TEST(name)                                                     \
    static void test_##name();                                         \
    static TestRegistrar registrar_##name(#name, test_##name);          \
    static void test_##name()

inline int &g_checks() { static int n = 0; return n; }
inline int &g_failures() { static int n = 0; return n; }

#define CHECK(cond)                                                     \
    do {                                                                \
        g_checks()++;                                                   \
        if (!(cond)) {                                                  \
            g_failures()++;                                             \
            fprintf(stderr, "  CHECK FAILED: %s (%s:%d)\n", #cond,      \
                    __FILE__, __LINE__);                                \
        }                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                  \
    do {                                                                \
        g_checks()++;                                                   \
        auto _a = (a);                                                  \
        auto _b = (b);                                                  \
        if (!(_a == _b)) {                                              \
            g_failures()++;                                             \
            fprintf(stderr, "  CHECK_EQ FAILED: %s != %s (%s:%d)\n",    \
                    #a, #b, __FILE__, __LINE__);                        \
        }                                                                \
    } while (0)

// Returns 0 if every check across every registered TEST() passed, 1
// otherwise — suitable as a process exit code for CI / make.
inline int run_all_tests() {
    int failed_tests = 0;
    for (auto &tc : test_registry()) {
        int before = g_failures();
        fprintf(stderr, "[ RUN  ] %s\n", tc.name.c_str());
        tc.fn();
        if (g_failures() > before) {
            failed_tests++;
            fprintf(stderr, "[ FAIL ] %s\n", tc.name.c_str());
        } else {
            fprintf(stderr, "[ OK   ] %s\n", tc.name.c_str());
        }
    }
    fprintf(stderr, "\n%d checks, %d failures, across %zu test cases (%d failed)\n",
            g_checks(), g_failures(), test_registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}