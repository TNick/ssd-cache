#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn fn;
};

std::vector<TestCase>& tests() {
    static std::vector<TestCase> value;
    return value;
}

}  // namespace

void register_test(const char* name, TestFn fn) {
    tests().push_back(TestCase{name, fn});
}

int main() {
    int failed = 0;
    for (const auto& test : tests()) {
        try {
            test.fn();
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAILED " << test.name << ": " << error.what() << "\n";
        } catch (...) {
            ++failed;
            std::cerr << "FAILED " << test.name << ": unknown exception\n";
        }
    }

    if (failed != 0) {
        return 1;
    }

    std::cout << "All tests passed (" << tests().size() << ")\n";
    return 0;
}
