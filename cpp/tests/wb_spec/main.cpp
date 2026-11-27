#include "spec.hpp"

#include <iostream>
#include <unordered_set>

namespace svwb::wb::spec {
void phase3_necromancy(Suite& suite);
void phase3_points(Suite& suite);
void phase3_world(Suite& suite);
void phase3_decks(Suite& suite);
}

int main(int argc, char** argv) {
    using namespace svwb::wb::spec;
    const std::string_view filter = argc >= 2 ? argv[1] : "";
    const bool exact = argc == 3 && std::string_view(argv[2]) == "--exact";
    if (argc > 3 || (argc == 3 && !exact)) {
        std::cerr << "usage: test_wb_spec [case-prefix|--list|--inventory] [--exact]\n";
        return 2;
    }
    Suite suite;
    catalog(suite);
    elf(suite);
    royal(suite);
    witch(suite);
    dragon(suite);
    nightmare(suite);
    bishop(suite);
    nemesis(suite);
    neutral(suite);
    special(suite);
    rules(suite);
    search_keys(suite);
    phase2_audit(suite);
    phase2_elf(suite);
    phase2_royal(suite);
    phase2_witch(suite);
    phase2_dragon(suite);
    phase2_nightmare(suite);
    phase2_bishop(suite);
    phase2_nemesis(suite);
    phase2_neutral(suite);
    phase2_special(suite);
    phase2_tokens(suite);
    phase3_necromancy(suite);
    phase3_points(suite);
    phase3_world(suite);
    phase3_decks(suite);
    std::unordered_set<std::string_view> names;
    int passed = 0;
    int failed = 0;
    for (const auto& test : suite.cases) {
        if (!names.insert(test.name).second) {
            std::cerr << "duplicate case: " << test.name << '\n';
            return 2;
        }
        if (filter == "--list") {
            std::cout << test.name << '\n';
            continue;
        }
        if (filter == "--inventory") {
            std::string_view file = test.registration.file_name();
            file.remove_prefix(file.find_last_of("/\\") + 1);
            std::cout << test.name << '\t' << file << ':' << test.registration.line() << '\n';
            continue;
        }
        if (exact ? test.name != filter : !test.name.starts_with(filter)) {
            continue;
        }
        try {
            test.run();
            ++passed;
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << test.name << " | " << error.what() << '\n';
        }
    }
    if (filter == "--list" || filter == "--inventory") {
        return 0;
    }
    std::cout << "SUMMARY pass=" << passed << " fail=" << failed << '\n';
    return passed + failed == 0 ? 2 : failed != 0;
}
