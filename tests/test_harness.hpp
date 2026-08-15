#pragma once

#include <iostream>
#include <string_view>

namespace minikv::test {

inline int failures = 0;

inline void Expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

inline int Finish(std::string_view suite_name) {
    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed in " << suite_name << '\n';
        return 1;
    }
    std::cout << "All " << suite_name << " tests passed\n";
    return 0;
}

}  // namespace minikv::test
