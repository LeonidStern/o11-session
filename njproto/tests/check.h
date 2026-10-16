// Tiny test framework: CHECK() records failures, TEST() registers cases.
#ifndef NJPROTO_TESTS_CHECK_H
#define NJPROTO_TESTS_CHECK_H

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testing {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> v;
    return v;
}
inline int& failures() {
    static int n = 0;
    return n;
}
inline const char*& currentCase() {
    static const char* c = "";
    return c;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { cases().push_back({name, std::move(fn)}); }
};

inline int runAll() {
    int ran = 0;
    for (auto& c : cases()) {
        currentCase() = c.name;
        int before = failures();
        c.fn();
        ++ran;
        std::printf("%s %s\n", failures() == before ? "PASS" : "FAIL", c.name);
    }
    std::printf("%d cases, %d failed checks\n", ran, failures());
    return failures() == 0 ? 0 : 1;
}

}  // namespace testing

#define TEST(name)                                                          \
    static void test_##name();                                              \
    static testing::Registrar reg_##name(#name, test_##name);               \
    static void test_##name()

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++testing::failures();                                          \
            std::printf("  CHECK failed in %s (%s:%d): %s\n", testing::currentCase(), __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

#define TEST_MAIN() int main() { return testing::runAll(); }

#endif
