#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
    int ran = 0;
    for (const auto& c : t::registry()) {
        if (argc > 1 && std::strstr(c.name, argv[1]) == nullptr) continue;
        int before = t::failures();
        c.fn();
        ++ran;
        std::cout << (t::failures() == before ? "  ok   " : "  FAIL ") << c.name << "\n";
    }
    std::cout << ran << " tests, " << t::failures() << " failures\n";
    return t::failures() == 0 ? 0 : 1;
}
