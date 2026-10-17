// Tiny test framework: CHECK() records failures, TEST() registers cases.
#ifndef O11PROTO_TESTS_CHECK_H
#define O11PROTO_TESTS_CHECK_H

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

// A CHECK that also ends the test case when it fails, for a condition the
// rest of the case relies on (an element it indexes, a message it decodes).
#define REQUIRE(cond)                                                       \
    do {                                                                    \
        bool require_ok_ = static_cast<bool>(cond);                         \
        CHECK(require_ok_ && #cond);                                        \
        if (!require_ok_) return;                                           \
    } while (0)

#define REQUIRE_EQ(a, b) REQUIRE((a) == (b))

// Line-buffered output, so the lines before a crash are not lost in the
// stdio buffer when the output goes to a pipe (ctest). The Windows C runtime
// treats _IOLBF as full buffering, so output is unbuffered there.
#ifdef _WIN32
#define TESTING_STDOUT_MODE _IONBF
#else
#define TESTING_STDOUT_MODE _IOLBF
#endif
#define TEST_MAIN()                                                         \
    int main() {                                                            \
        std::setvbuf(stdout, nullptr, TESTING_STDOUT_MODE, BUFSIZ);         \
        return testing::runAll();                                           \
    }

#endif
