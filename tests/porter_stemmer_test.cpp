// porter_stemmer_test.cpp — targeted edge-case tests for the Porter stemmer
// tests critical paths affecting BM25 search accuracy

#include <iostream>
#include <string>
#include <vector>
#include "../v8.0/porter_stemmer.hpp"

struct TestCase {
    std::string input;
    std::string expected;
};

int main() {
    std::vector<TestCase> cases = {
        // standard suffix stripping
        {"university",  "univers"},
        {"universal",   "univers"},
        {"organization","organ"},
        {"caresses",    "caress"},
        {"ponies",      "poni"},
        {"ties",        "ti"},
        {"cats",        "cat"},

        // -ed / -ing removal
        {"feed",        "feed"},
        {"agreed",      "agre"},
        {"disabled",    "disabl"},
        {"matting",     "mat"},
        {"mating",      "mate"},     // tricky pair with matting
        {"meeting",     "meet"},
        {"milling",     "mill"},
        {"messing",     "mess"},
        {"meetings",    "meet"},

        // edge cases
        {"",            ""},          // empty string
        {"a",           "a"},         // single char
        {"cat",         "cat"},       // already stemmed
    };

    int pass = 0, fail = 0;

    for (const auto& tc : cases) {
        std::string result = porter::stem(tc.input);
        if (result == tc.expected) {
            std::cout << "PASS: \"" << tc.input << "\" -> \"" << result << "\"" << std::endl;
            pass++;
        } else {
            std::cout << "FAIL: \"" << tc.input << "\" -> \"" << result
                      << "\" (expected \"" << tc.expected << "\")" << std::endl;
            fail++;
        }
    }

    std::cout << "\n--- Results: " << pass << " passed, " << fail << " failed ---" << std::endl;
    return fail > 0 ? 1 : 0;
}
