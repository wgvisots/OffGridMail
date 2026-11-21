// Minimal self-registering test harness (no third-party dependencies).
#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::cerr << file << ":" << line << ": FAILED " << what << "\n";
}

}  // namespace t

#define T_CAT2(a, b) a##b
#define T_CAT(a, b) T_CAT2(a, b)
#define TEST(name)                                              \
    static void T_CAT(test_, name)();                           \
    static ::t::Reg T_CAT(reg_, name)(#name, T_CAT(test_, name)); \
    static void T_CAT(test_, name)()

#define CHECK(cond) \
    do { if (!(cond)) ::t::fail(__FILE__, __LINE__, #cond); } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        const auto va_ = (a);  /* copies: (a) may refer into a temporary */        \
        const auto vb_ = (b);                                                       \
        if (!(va_ == vb_))                                                          \
            ::t::fail(__FILE__, __LINE__, std::string(#a " == " #b));               \
    } while (0)

// Stops the current test when a precondition fails (e.g. an optional is empty).
#define REQUIRE(cond) \
    do { if (!(cond)) { ::t::fail(__FILE__, __LINE__, #cond); return; } } while (0)
