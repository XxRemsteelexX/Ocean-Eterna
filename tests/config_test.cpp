// config_test.cpp — tests JSON config parsing: valid, defaults, invalid JSON
// uses nlohmann/json + config.hpp

#include <iostream>
#include <fstream>
#include <string>
#include <cstdio>
#include "../json.hpp"
#include "../v9.0/config.hpp"

int main() {
    int pass = 0, fail = 0;

    auto check = [&](const std::string& name, bool cond) {
        if (cond) { std::cout << "PASS: " << name << std::endl; pass++; }
        else      { std::cout << "FAIL: " << name << std::endl; fail++; }
    };

    // --- test 1: valid config with partial overrides ---
    {
        const std::string path = "/tmp/oe_test_config_valid.json";
        std::ofstream f(path);
        f << R"({
            "server": { "port": 9999, "host": "127.0.0.1" },
            "search": { "top_k": 16, "bm25_k1": 2.0 },
            "reranker": { "enabled": true, "candidate_count": 100 }
        })";
        f.close();

        Config cfg = load_config(path);
        check("valid: server.port == 9999", cfg.server.port == 9999);
        check("valid: server.host == 127.0.0.1", cfg.server.host == "127.0.0.1");
        check("valid: search.top_k == 16", cfg.search.top_k == 16);
        check("valid: search.k1 == 2.0", cfg.search.k1 == 2.0);
        check("valid: search.b == 0.75 (default)", cfg.search.b == 0.75);
        check("valid: reranker.enabled == true", cfg.reranker.enabled == true);
        check("valid: reranker.candidate_count == 100", cfg.reranker.candidate_count == 100);
        check("valid: reranker.url == default", cfg.reranker.url == "http://127.0.0.1:8889/rerank");

        std::remove(path.c_str());
    }

    // --- test 2: missing file -> all defaults ---
    {
        Config cfg = load_config("/tmp/oe_test_config_DOES_NOT_EXIST.json");
        check("missing: server.port == 8888 (default)", cfg.server.port == 8888);
        check("missing: server.host == 0.0.0.0 (default)", cfg.server.host == "0.0.0.0");
        check("missing: search.top_k == 8 (default)", cfg.search.top_k == 8);
        check("missing: search.k1 == 1.5 (default)", cfg.search.k1 == 1.5);
        check("missing: search.b == 0.75 (default)", cfg.search.b == 0.75);
        check("missing: llm.use_external == true (default)", cfg.llm.use_external == true);
        check("missing: reranker.enabled == false (default)", cfg.reranker.enabled == false);
        check("missing: scanner.enabled == false (default)", cfg.scanner.enabled == false);
        check("missing: scanner.batch_size == 8 (default)", cfg.scanner.batch_size == 8);
    }

    // --- test 3: invalid JSON -> defaults (with warning) ---
    {
        const std::string path = "/tmp/oe_test_config_invalid.json";
        std::ofstream f(path);
        f << "{ this is not valid json!!!";
        f.close();

        std::cout << "  (expecting a parse warning below)" << std::endl;
        Config cfg = load_config(path);
        check("invalid: server.port == 8888 (default)", cfg.server.port == 8888);
        check("invalid: search.top_k == 8 (default)", cfg.search.top_k == 8);
        check("invalid: reranker.enabled == false (default)", cfg.reranker.enabled == false);

        std::remove(path.c_str());
    }

    // --- test 4: empty object {} -> all defaults ---
    {
        const std::string path = "/tmp/oe_test_config_empty.json";
        std::ofstream f(path);
        f << "{}";
        f.close();

        Config cfg = load_config(path);
        check("empty obj: server.port == 8888 (default)", cfg.server.port == 8888);
        check("empty obj: search.top_k == 8 (default)", cfg.search.top_k == 8);
        check("empty obj: llm.timeout_sec == 30 (default)", cfg.llm.timeout_sec == 30);

        std::remove(path.c_str());
    }

    std::cout << "\n--- Results: " << pass << " passed, " << fail << " failed ---" << std::endl;
    return fail > 0 ? 1 : 0;
}
