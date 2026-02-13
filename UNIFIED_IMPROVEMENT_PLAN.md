# OceanEterna - Unified Improvement Plan

**Date:** February 1, 2026
**Sources:** Combined analysis from OpenCode and Claude (Anthropic)
**Version:** v3.0

---

## Executive Summary

This plan synthesizes insights from two comprehensive analyses to provide a complete roadmap for transforming OceanEterna from a solid v3 into a **production-ready, high-performance RAG system**.

### Combined Assessment

**Strengths:**
- ✅ Binary manifest: 3.2x faster startup (41s → 13s)
- ✅ 100% accuracy on test queries
- ✅ Innovative conversation features (caching, self-referential search)
- ✅ Zero API costs, CPU-only operation
- ✅ Excellent memory efficiency vs commercial solutions

**Critical Issues Identified by Both Analyses:**

| Issue | OpenCode Analysis | Claude Analysis | Priority |
|--------|------------------|----------------|-----------|
| Code structure | Large files, cleanup needed | 2200 lines in one file | 🔴 High |
| Hardcoded config | API key in source | API URL, model hardcoded | 🔴 High |
| Search speed | 700ms, needs BlockWeakAnd | BM25 alone, needs stemming | 🔴 High |
| Memory usage | 8 GB, needs mmap | 3 GB inverted index, needs mmap | 🔴 High |
| HTTP implementation | - | Raw sockets, fragile | 🔴 High |
| Error handling | - | No LLM retry logic | 🔴 High |
| Production features | - | No auth, rate limiting, graceful shutdown | 🔴 High |
| Semantic search | BM25 only, misses semantic | No stemming, no synonym expansion | 🟡 Medium |
| Index updates | - | No incremental updates | 🟡 Medium |
| Testing | 10 test cases | Need negative, edge, load tests | 🟡 Medium |

**Unified Rating:**
- **Architecture:** 7/10 (good foundation, needs modularization)
- **Performance:** 7/10 (fast startup, slow search, memory issues)
- **Code Quality:** 6/10 (works but monolithic, hardcoded)
- **Production Ready:** 4/10 (missing auth, error handling, logging)
- **Overall:** 6/10 - Solid research project, needs production hardening

---

## Unified Improvement Roadmap

### Phase 1: Critical Production Readiness (1-2 weeks) 🔴

**Goal:** Transform from research project to production-grade system

#### 1.1 HTTP Server Refactor
**Issues:** Raw sockets, fragile parsing, no CORS handling

**Solution:** Replace with lightweight HTTP library

**Options:**
- **cpp-httplib** (Recommended)
  - Header-only, ~3000 lines
  - MIT licensed
  - Production-tested
  - Built-in CORS, chunked encoding, Content-Length parsing

- **crow**
  - Header-only
  - Modern C++14
  - Routing DSL

**Implementation:**
```cpp
// Replace current HTTP handling (lines 1700-1800 in ocean_chat_server.cpp)
#include <httplib.h>

// Simple server setup
httplib::Server svr;

svr.Post("/chat", [](const httplib::Request& req, httplib::Response& res) {
    // req.body automatically parsed
    // Content-Length handled by library
    // CORS handled by library
    json body = json::parse(req.body);
    string question = body["question"];
    json response = handle_chat(question);
    res.set_content(response.dump(), "application/json");
});

// CORS handling
svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    return httplib::Server::HandlerResponse::Unhandled;
});

svr.listen("0.0.0.0", 8888);
```

**Benefits:**
- Eliminates ~200 lines of fragile HTTP parsing code
- Fixes edge cases (chunked encoding, Content-Length validation)
- Built-in CORS support
- Security: library-tested, no buffer overflow risks

**Effort:** 2-3 days
**Risk:** Low (well-tested library)
**Impact:** Removes critical production risk

---

#### 1.2 Code Modularization
**Issues:** 2200 lines in ocean_chat_server.cpp, multiple responsibilities

**Solution:** Split into focused modules

**New Structure:**
```
chat/
├── ocean_chat_server.cpp      // Main entry point (100 lines)
├── http_server.hpp            // HTTP handling (300 lines)
├── http_server.cpp
├── search_engine.hpp          // BM25 search (400 lines)
├── search_engine.cpp
├── llm_client.hpp            // LLM API integration (200 lines)
├── llm_client.cpp
├── conversation.hpp          // Turn management (300 lines)
├── conversation.cpp
├── corpus_manager.hpp         // Manifest & storage (200 lines)
├── corpus_manager.cpp
├── config.hpp                // Configuration management (100 lines)
├── config.cpp
└── utils.hpp                 // Utilities (150 lines)
    utils.cpp
```

**Benefits:**
- Easier to test each component
- Parallel development
- Code reuse across projects
- Clear separation of concerns

**Effort:** 2-3 days
**Risk:** Medium (refactoring, need testing)
**Impact:** Long-term maintainability

---

#### 1.3 Configuration Externalization
**Issues:** Hardcoded API URL, model name, API key

**Solution:** Config file + environment variables

**Implementation:**

**Option A: JSON config file** (Recommended)
```json
{
  "server": {
    "host": "0.0.0.0",
    "port": 8888,
    "workers": 4
  },
  "llm": {
    "api_url": "https://routellm.abacus.ai/v1/chat/completions",
    "model": "gpt-5-mini",
    "api_key_env": "OCEAN_ETERNA_API_KEY",
    "timeout_ms": 30000,
    "max_retries": 3,
    "retry_backoff_ms": 1000
  },
  "search": {
    "top_k": 8,
    "bm25_k1": 1.2,
    "bm25_b": 0.75
  },
  "corpus": {
    "manifest_path": "build_out/manifest_guten9m.jsonl",
    "storage_path": "build_out/storage/guten9m.bin",
    "max_conversations": 1000,
    "conversation_ttl_hours": 24
  }
}
```

**Option B: Environment variables only**
```bash
export OCEAN_SERVER_HOST=0.0.0.0
export OCEAN_SERVER_PORT=8888
export OCEAN_LLM_API_URL=https://routellm.abacus.ai/v1/chat/completions
export OCEAN_LLM_MODEL=gpt-5-mini
export OCEAN_LLM_API_KEY=your-key-here
export OCEAN_SEARCH_TOP_K=8
```

**Config Loader:**
```cpp
// config.hpp
struct Config {
    struct Server {
        string host = "0.0.0.0";
        int port = 8888;
        int workers = 4;
    } server;

    struct LLM {
        string api_url;
        string model;
        string api_key_env = "OCEAN_ETERNA_API_KEY";
        int timeout_ms = 30000;
        int max_retries = 3;
        int retry_backoff_ms = 1000;
    } llm;

    struct Search {
        int top_k = 8;
        double bm25_k1 = 1.2;
        double bm25_b = 0.75;
    } search;

    struct Corpus {
        string manifest_path;
        string storage_path;
        int max_conversations = 1000;
        int conversation_ttl_hours = 24;
    } corpus;
};

Config load_config(const string& path) {
    Config config;
    std::ifstream in(path);
    if (in) {
        json j = json::parse(in);
        config.server.host = j.value("host", config.server.host);
        config.server.port = j.value("port", config.server.port);
        // ... load all fields
    }

    // Override with environment variables
    if (const char* env = getenv("OCEAN_LLM_API_KEY")) {
        config.llm.api_key = env;
    }
    // ... check other env vars

    return config;
}
```

**Benefits:**
- No recompilation to change settings
- Environment-specific configs (dev/staging/prod)
- Secrets managed via environment variables

**Effort:** 1-2 days
**Risk:** Low
**Impact:** Deployment flexibility, security

---

#### 1.4 LLM Error Handling & Retry Logic
**Issues:** No retry logic, no exponential backoff, no fallback

**Solution:** Robust error handling with retries

**Implementation:**
```cpp
// llm_client.hpp
#include <chrono>
#include <thread>

class LLMClient {
public:
    struct Response {
        bool success;
        string content;
        double latency_ms;
        string error;
    };

    Response query_with_retry(const string& prompt, int max_retries = 3) {
        for (int attempt = 0; attempt < max_retries; attempt++) {
            auto start = chrono::high_resolution_clock::now();

            try {
                Response resp = send_request(prompt);

                if (resp.success) {
                    auto end = chrono::high_resolution_clock::now();
                    resp.latency_ms = duration<double, milli>(end - start).count();
                    return resp;
                }

                // Check if error is retryable
                if (is_retryable_error(resp.error)) {
                    cerr << "LLM request failed (attempt " << (attempt + 1)
                         << "/" << max_retries << "): " << resp.error << endl;

                    if (attempt < max_retries - 1) {
                        // Exponential backoff
                        int delay_ms = config.llm.retry_backoff_ms * (1 << attempt);
                        cerr << "Retrying in " << delay_ms << "ms..." << endl;
                        this_thread::sleep_for(chrono::milliseconds(delay_ms));
                    }
                } else {
                    // Non-retryable error, return immediately
                    return resp;
                }
            } catch (const exception& e) {
                cerr << "LLM request exception: " << e.what() << endl;

                if (attempt < max_retries - 1) {
                    int delay_ms = config.llm.retry_backoff_ms * (1 << attempt);
                    this_thread::sleep_for(chrono::milliseconds(delay_ms));
                }
            }
        }

        // All retries exhausted
        return {false, "", 0, "Max retries exceeded"};
    }

private:
    bool is_retryable_error(const string& error) {
        // Retry on: timeouts, rate limits, server errors (5xx)
        return error.find("timeout") != string::npos ||
               error.find("rate limit") != string::npos ||
               error.find("502") != string::npos ||
               error.find("503") != string::npos ||
               error.find("504") != string::npos;
    }

    Response send_request(const string& prompt);
};
```

**Circuit Breaker Pattern:**
```cpp
class CircuitBreaker {
public:
    CircuitBreaker(int failure_threshold = 5, int timeout_seconds = 60)
        : failure_threshold_(failure_threshold), timeout_(timeout_seconds) {}

    bool should_attempt() {
        if (state_ == State::OPEN) {
            auto now = chrono::system_clock::now();
            if (now - last_failure_time_ > chrono::seconds(timeout_)) {
                state_ = State::HALF_OPEN;
                return true;
            }
            return false;
        }
        return true;
    }

    void record_success() {
        failure_count_ = 0;
        state_ = State::CLOSED;
    }

    void record_failure() {
        failure_count_++;
        if (failure_count_ >= failure_threshold_) {
            state_ = State::OPEN;
            last_failure_time_ = chrono::system_clock::now();
            cerr << "Circuit breaker opened after " << failure_count_
                 << " failures" << endl;
        }
    }

private:
    enum class State { CLOSED, OPEN, HALF_OPEN };
    State state_ = State::CLOSED;
    int failure_count_ = 0;
    int failure_threshold_;
    int timeout_;
    chrono::system_clock::time_point last_failure_time_;
};
```

**Fallback to Local LLM:**
```cpp
class LLMClient {
private:
    CircuitBreaker breaker_;

public:
    Response query_with_fallback(const string& prompt) {
        if (!breaker_.should_attempt()) {
            // Circuit open, use fallback immediately
            return query_local_llm(prompt);
        }

        Response resp = query_with_retry(prompt);
        if (resp.success) {
            breaker_.record_success();
            return resp;
        } else {
            breaker_.record_failure();
            // Fallback to local LLM
            cerr << "External LLM failed, using local fallback" << endl;
            return query_local_llm(prompt);
        }
    }

private:
    Response query_local_llm(const string& prompt) {
        // Integrate llama.cpp or similar
        // For now, return error message
        return {false, "External LLM unavailable, no local fallback configured", 0};
    }
};
```

**Benefits:**
- Resilient to network issues
- Exponential backoff prevents thundering herd
- Circuit breaker prevents cascading failures
- Fallback option ensures service continues

**Effort:** 2-3 days
**Risk:** Medium (complexity)
**Impact:** Production reliability

---

#### 1.5 Production Basics
**Issues:** No auth, rate limiting, request logging, graceful shutdown

**Solution:** Add production-grade infrastructure

**1.5.1 Authentication**
```cpp
// Add API key authentication
svr.set_pre_routing_handler([&config](const httplib::Request& req, httplib::Response& res) {
    // Skip auth for health check
    if (req.path == "/health") {
        return httplib::Server::HandlerResponse::Unhandled;
    }

    // Check API key
    auto it = req.headers.find("X-API-Key");
    if (it == req.headers.end()) {
        res.status = 401;
        res.set_content("Missing API key", "text/plain");
        return httplib::Server::HandlerResponse::Handled;
    }

    string provided_key = it->second;
    if (provided_key != config.auth.api_key) {
        res.status = 403;
        res.set_content("Invalid API key", "text/plain");
        return httplib::Server::HandlerResponse::Handled;
    }

    return httplib::Server::HandlerResponse::Unhandled;
});
```

**1.5.2 Rate Limiting**
```cpp
#include <unordered_map>
#include <chrono>

class RateLimiter {
public:
    RateLimiter(int requests_per_minute = 60)
        : requests_per_minute_(requests_per_minute) {}

    bool is_allowed(const string& client_ip) {
        auto now = chrono::system_clock::now();
        auto& bucket = buckets_[client_ip];

        // Clean old entries
        bucket.erase(
            remove_if(bucket.begin(), bucket.end(),
                [now](const auto& ts) {
                    return now - ts > chrono::minutes(1);
                }),
            bucket.end()
        );

        if (bucket.size() >= requests_per_minute_) {
            return false; // Rate limited
        }

        bucket.push_back(now);
        return true;
    }

private:
    int requests_per_minute_;
    unordered_map<string, vector<chrono::system_clock::time_point>> buckets_;
};

// Use in handler
svr.Post("/chat", [&rate_limiter](const httplib::Request& req, httplib::Response& res) {
    string client_ip = req.remote_addr;
    if (!rate_limiter.is_allowed(client_ip)) {
        res.status = 429;
        res.set_content("Rate limit exceeded", "text/plain");
        res.set_header("Retry-After", "60");
        return;
    }
    // ... process request
});
```

**1.5.3 Request Logging**
```cpp
#include <fstream>
#include <sstream>

class RequestLogger {
public:
    void log(const string& method, const string& path,
             const string& client_ip, int status,
             double latency_ms, size_t body_size) {
        auto now = chrono::system_clock::now();
        time_t tt = chrono::system_clock::to_time_t(now);
        tm tm = *std::localtime(&tt);

        char buffer[80];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);

        // JSON format for easy parsing
        json log_entry = {
            {"timestamp", buffer},
            {"method", method},
            {"path", path},
            {"client_ip", client_ip},
            {"status", status},
            {"latency_ms", latency_ms},
            {"body_size", body_size}
        };

        log_file_ << log_entry.dump() << endl;
    }

private:
    ofstream log_file_ = ofstream("requests.log", ios::app);
};

// Use in handlers
svr.Post("/chat", [&](const httplib::Request& req, httplib::Response& res) {
    auto start = chrono::high_resolution_clock::now();

    // ... process request ...

    auto end = chrono::high_resolution_clock::now();
    double latency_ms = duration<double, milli>(end - start).count();
    logger.log("POST", "/chat", req.remote_addr, res.status, latency_ms, req.body.size());
});
```

**1.5.4 Graceful Shutdown**
```cpp
#include <csignal>
#include <atomic>

atomic<bool> shutdown_requested(false);

void signal_handler(int signal) {
    cout << "\nReceived signal " << signal << ", shutting down gracefully..." << endl;
    shutdown_requested = true;
}

int main() {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    httplib::Server svr;
    // ... setup server ...

    // Start server in background thread
    thread server_thread([&]() {
        svr.listen("0.0.0.0", 8888);
    });

    // Main thread waits for shutdown signal
    while (!shutdown_requested) {
        this_thread::sleep_for(chrono::milliseconds(100));
    }

    // Graceful shutdown
    cout << "Stopping server..." << endl;
    svr.stop();
    server_thread.join();

    // Save any in-memory state
    cout << "Saving conversation history..." << endl;
    // conversation_manager.save();

    cout << "Shutdown complete" << endl;
    return 0;
}
```

**Benefits:**
- Production-ready security
- Prevents abuse with rate limiting
- Debugging with request logs
- Clean shutdown, no data loss

**Effort:** 3-4 days
**Risk:** Medium
**Impact:** Production deployment

---

### Phase 2: Performance Optimization (1-2 weeks) 🔴

**Goal:** Improve search speed and memory efficiency

#### 2.1 Memory-Mapped Manifests
**Issues:** 8 GB RAM for manifest, all loaded at startup

**Solution:** Use mmap for on-demand loading

**Implementation:**
```cpp
// corpus_manager.cpp
#include <sys/mman.h>
#include <fcntl.h>

class CorpusManager {
public:
    bool load_manifest_mmap(const string& path) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            cerr << "Failed to open manifest: " << strerror(errno) << endl;
            return false;
        }

        // Get file size
        struct stat st;
        if (fstat(fd, &st) < 0) {
            close(fd);
            return false;
        }
        size_t file_size = st.st_size;

        // Memory-map the file
        void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped == MAP_FAILED) {
            cerr << "Failed to mmap manifest: " << strerror(errno) << endl;
            close(fd);
            return false;
        }

        // Keep file descriptor open
        manifest_fd_ = fd;
        manifest_mapped_ = mapped;
        manifest_size_ = file_size;

        // Parse header only
        BinaryManifestHeader* header = (BinaryManifestHeader*)mapped;
        chunk_count_ = header->chunk_count;
        keyword_count_ = header->keyword_count;

        // Build index for O(1) access
        build_chunk_index(header);

        return true;
    }

    DocMeta get_chunk_by_index(uint32_t index) {
        // Access on-demand, OS handles paging
        uint8_t* base = (uint8_t*)manifest_mapped_;
        // ... calculate offset and read chunk ...
    }

private:
    int manifest_fd_ = -1;
    void* manifest_mapped_ = nullptr;
    size_t manifest_size_ = 0;
};
```

**Benefits:**
- 60-80% RAM reduction (8GB → 1.6-3.2GB)
- Faster startup (only load header)
- OS handles caching efficiently

**Effort:** 1-2 days
**Risk:** Low (well-tested technology)
**Impact:** Memory efficiency, startup speed

---

#### 2.2 Zstd Compression
**Issues:** LZ4 has 2.68x ratio, could be better

**Solution:** Switch to Zstd for better compression

**Implementation:**
```cpp
// ocean_build.cpp - During indexing
#ifdef ZSTD
static std::string compress_zstd(const std::string& input, int level) {
    size_t bound = ZSTD_compressBound(input.size());
    std::string out(bound, '\0');
    size_t written = ZSTD_compress(out.data(), out.size(),
                                   input.data(), input.size(),
                                   level);
    if (ZSTD_isError(written)) {
        throw std::runtime_error("zstd compress failed");
    }
    out.resize(written);
    return out;
}
#endif

// ocean_chat_server.cpp - During search
#ifdef ZSTD
static std::string decompress_zstd(const std::string& compressed) {
    size_t bound = ZSTD_getDecompressedSize(compressed.data(), compressed.size());
    if (bound == ZSTD_CONTENTSIZE_UNKNOWN) {
        bound = 65536; // Fallback max size
    }
    std::string out(bound, '\0');
    size_t written = ZSTD_decompress(out.data(), out.size(),
                                    compressed.data(), compressed.size());
    if (ZSTD_isError(written)) {
        throw std::runtime_error("zstd decompress failed");
    }
    out.resize(written);
    return out;
}
#endif
```

**Benefits:**
- 2x better compression (2.68x → 4.0x)
- 33% disk savings (10.2 GB → 6.8 GB)
- Negligible query impact (0.8ms → 2.5ms)

**Effort:** 2-3 hours
**Risk:** Low
**Impact:** Storage efficiency

---

#### 2.3 BlockWeakAnd Early Termination
**Issues:** Scores all 5M docs, even when only top-8 needed

**Solution:** Skip unpromising document blocks

**Implementation:**
```cpp
// search_engine.hpp
class BlockWeakAndSearch {
public:
    void build_blocks(const Corpus& corpus, int block_size = 128) {
        int num_blocks = (corpus.docs.size() + block_size - 1) / block_size;
        block_max_scores_.resize(num_blocks);

        // Pre-compute max BM25 contribution per block per term
        #pragma omp parallel for
        for (int block_id = 0; block_id < num_blocks; block_id++) {
            for (const auto& [term, postings] : corpus.inverted_index) {
                float max_score = 0;
                for (uint32_t doc_id : postings) {
                    if (doc_id >= block_id * block_size &&
                        doc_id < (block_id + 1) * block_size) {
                        float score = compute_bm25_for_doc(corpus, doc_id, term);
                        max_score = std::max(max_score, score);
                    }
                }
                block_max_scores_[block_id][term] = max_score;
            }
        }
    }

    vector<Hit> search(const Corpus& corpus, const vector<string>& query_terms, int topk) {
        // Convert query terms to term IDs
        vector<string> term_ids = extract_term_ids(query_terms);

        vector<pair<uint32_t, float>> doc_scores(corpus.docs.size());

        // BlockWeakAnd algorithm
        float threshold = 0;
        for (int block_id = 0; block_id < num_blocks_; block_id++) {
            // Calculate max possible score for this block
            float block_max = 0;
            for (const string& term_id : term_ids) {
                block_max += block_max_scores_[block_id][term_id];
            }

            // Skip blocks that can't reach threshold
            if (block_max < threshold) {
                continue; // SKIP
            }

            // Score documents in this block
            int start = block_id * block_size_;
            int end = std::min(start + block_size_, (int)corpus.docs.size());
            for (int doc_id = start; doc_id < end; doc_id++) {
                float score = 0;
                for (const string& term_id : term_ids) {
                    score += compute_bm25_for_doc(corpus, doc_id, term_id);
                }
                doc_scores[doc_id] = {doc_id, score};
            }

            // Update threshold (top-k so far)
            if (doc_scores.size() >= topk) {
                partial_sort(doc_scores.begin(),
                           doc_scores.begin() + topk,
                           doc_scores.end(),
                           [](auto& a, auto& b) { return a.second > b.second; });
                threshold = doc_scores[topk - 1].second;
            }
        }

        // Return top-k
        sort(doc_scores.begin(), doc_scores.end(),
             [](auto& a, auto& b) { return a.second > b.second; });
        doc_scores.resize(topk);

        vector<Hit> results;
        for (auto& [doc_id, score] : doc_scores) {
            results.push_back({doc_id, score});
        }
        return results;
    }

private:
    int block_size_ = 128;
    int num_blocks_;
    vector<unordered_map<string, float>> block_max_scores_;
};
```

**Benefits:**
- 2-5x faster search (700ms → 140-350ms)
- Works with existing index structure
- Minimal memory overhead

**Effort:** 2-3 days
**Risk:** Medium (algorithm complexity)
**Impact:** Search speed

---

#### 2.4 SIMD Vectorization
**Issues:** No AVX/AVX-512 for vector operations

**Solution:** Add SIMD for TF-IDF calculations

**Implementation:**
```cpp
// utils.hpp
#include <immintrin.h>

namespace SIMD {

// Detect CPU features at runtime
bool has_avx2() {
    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;
    __cpuid(regs, 7);
    return (regs[1] & (1 << 5)) != 0;
}

bool has_avx512() {
    int regs[4];
    __cpuid(regs, 0);
    if (regs[0] < 7) return false;
    __cpuid(regs, 7);
    return (regs[1] & (1 << 16)) != 0;
}

// Vectorized dot product (AVX2)
float dot_product_avx2(const float* a, const float* b, int n) {
    __m256 sum = _mm256_setzero_ps();
    int i = 0;

    // Process 8 floats at a time
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_load_ps(&a[i]);
        __m256 vb = _mm256_load_ps(&b[i]);
        sum = _mm256_add_ps(sum, _mm256_mul_ps(va, vb));
    }

    // Horizontal sum
    float tmp[8];
    _mm256_store_ps(tmp, sum);
    float result = tmp[0] + tmp[1] + tmp[2] + tmp[3] +
                  tmp[4] + tmp[5] + tmp[6] + tmp[7];

    // Handle remaining elements
    for (; i < n; i++) {
        result += a[i] * b[i];
    }

    return result;
}

// Vectorized dot product (AVX-512)
float dot_product_avx512(const float* a, const float* b, int n) {
    __m512 sum = _mm512_setzero_ps();
    int i = 0;

    // Process 16 floats at a time
    for (; i + 16 <= n; i += 16) {
        __m512 va = _mm512_load_ps(&a[i]);
        __m512 vb = _mm512_load_ps(&b[i]);
        sum = _mm512_add_ps(sum, _mm512_mul_ps(va, vb));
    }

    // Horizontal sum
    float result = _mm512_reduce_add_ps(sum);

    // Handle remaining elements
    for (; i < n; i++) {
        result += a[i] * b[i];
    }

    return result;
}

// Generic wrapper
float dot_product(const float* a, const float* b, int n) {
    if (has_avx512() && n >= 16) {
        return dot_product_avx512(a, b, n);
    } else if (has_avx2() && n >= 8) {
        return dot_product_avx2(a, b, n);
    } else {
        // Scalar fallback
        float sum = 0;
        for (int i = 0; i < n; i++) {
            sum += a[i] * b[i];
        }
        return sum;
    }
}
} // namespace SIMD
```

**Benefits:**
- 2-4x speedup on vectorizable code
- No memory overhead
- Backward compatible (runtime detection)

**Effort:** 1-2 days
**Risk:** Low (feature detection available)
**Impact:** Search speed

---

### Phase 3: Advanced Search (2-3 weeks) 🟡

**Goal:** Improve retrieval accuracy and semantic coverage

#### 3.1 Porter Stemmer
**Issues:** No stemming, "ocean mammals" won't match "ocean mammal"

**Solution:** Add Porter stemmer for term normalization

**Implementation:**
```cpp
// stemmer.hpp (Porter stemmer, ~200 lines)
class PorterStemmer {
public:
    static string stem(const string& word) {
        string w = word;
        w = step1ab(w);
        w = step1c(w);
        w = step2(w);
        w = step3(w);
        w = step4(w);
        w = step5(w);
        return w;
    }

private:
    static bool consonant(char c) {
        return !(c == 'a' || c == 'e' || c == 'i' ||
                c == 'o' || c == 'u');
    }

    static bool vowel(char c) {
        return !consonant(c);
    }

    // ... implement Porter rules ...
};

// Use in search
vector<string> extract_query_terms(const string& query) {
    vector<string> tokens = split_tokens(query);
    vector<string> stemmed;

    for (const string& token : tokens) {
        string lower = to_lower(token);
        string stemmed = PorterStemmer::stem(lower);
        stemmed.push_back(stemmed);
    }

    return stemmed;
}

// Also stem during indexing
void build_corpus(const vector<DocMeta>& docs) {
    for (const auto& doc : docs) {
        for (const string& kw : doc.keywords) {
            string stemmed = PorterStemmer::stem(to_lower(kw));
            inverted_index[stemmed].push_back(doc_id);
        }
    }
}
```

**Benefits:**
- "ocean mammals" matches "ocean mammal"
- "running" matches "run"
- +5-10% recall improvement
- ~200 lines of code, fast execution

**Effort:** 1-2 days
**Risk:** Low (well-studied algorithm)
**Impact:** Recall accuracy

---

#### 3.2 Improved Keyword Extraction
**Issues:** Drops 2-char terms (AI, ML, US), basic stop words

**Solution:** Enhanced extraction with NLP techniques

**Implementation:**
```cpp
// search_engine.hpp
class KeywordExtractor {
public:
    vector<string> extract(const string& text, int max_keywords = 40) {
        vector<string> tokens = tokenize(text);
        vector<string> keywords;

        unordered_set<string> seen;

        for (const string& token : tokens) {
            // Allow 1-2 char terms if they're meaningful
            if (token.length() < 1 || token.length() > 20) continue;

            // Better stop word detection (including domain-specific)
            if (is_stop_word(token)) continue;

            // Stem the term
            string stemmed = PorterStemmer::stem(to_lower(token));

            // Handle compound words
            vector<string> ngrams = extract_ngrams(tokens, token_position, 2, 3);
            for (const string& ngram : ngrams) {
                if (!seen.count(ngram)) {
                    seen.insert(ngram);
                    keywords.push_back(ngram);
                }
            }

            if (!seen.count(stemmed) && is_valid_token(stemmed)) {
                seen.insert(stemmed);
                keywords.push_back(stemmed);
            }

            if (keywords.size() >= max_keywords) break;
        }

        return keywords;
    }

private:
    bool is_stop_word(const string& token) {
        static const unordered_set<string> stop_words = {
            // English
            "the", "and", "for", "are", "but", "not", "you", "all", "can", "her",
            "was", "one", "our", "out", "day", "get", "has", "him", "his",
            // Allow 2-char meaningful terms
            // "ai", "ml", "us", "uk", "io", "os", "db"
        };
        return stop_words.count(to_lower(token));
    }

    vector<string> extract_ngrams(const vector<string>& tokens, int pos,
                                  int min_n, int max_n) {
        vector<string> ngrams;
        for (int n = min_n; n <= max_n; n++) {
            if (pos + n > tokens.size()) break;
            string ngram;
            for (int i = 0; i < n; i++) {
                if (i > 0) ngram += " ";
                ngram += tokens[pos + i];
            }
            ngrams.push_back(ngram);
        }
        return ngrams;
    }

    bool is_valid_token(const string& token) {
        // Allow alphanumeric + common abbreviations
        for (char c : token) {
            if (!isalnum(c) && c != '-' && c != '\'') {
                return false;
            }
        }
        return true;
    }
};
```

**Benefits:**
- Captures 2-char terms (AI, ML, US, UK)
- N-grams capture compound words
- Better stop word handling
- +3-5% recall improvement

**Effort:** 1-2 days
**Risk:** Low
**Impact:** Recall accuracy

---

#### 3.3 Hybrid Search with RRF Fusion
**Issues:** BM25-only misses semantic matches

**Solution:** Combine BM25 + lightweight embeddings

**Implementation:**
```cpp
// hybrid_search.hpp
class HybridSearch {
public:
    void initialize_embeddings(const Corpus& corpus) {
        // Pre-compute embeddings for all chunks
        // Use MiniLM-L6 (80MB model, fast)
        embeddings_.resize(corpus.docs.size());

        #pragma omp parallel for
        for (size_t i = 0; i < corpus.docs.size(); i++) {
            embeddings_[i] = embed_model_->encode(corpus.docs[i].summary);
        }
    }

    vector<Hit> search_hybrid(const Corpus& corpus, const string& query, int topk) {
        // BM25 search
        vector<Hit> bm25_results = search_bm25(corpus, query, topk * 2);

        // Embedding search
        vector<float> query_embedding = embed_model_->encode(query);
        vector<Hit> semantic_results = search_embeddings(query_embedding, topk * 2);

        // RRF fusion
        return rrf_fusion(bm25_results, semantic_results, topk);
    }

private:
    vector<Hit> rrf_fusion(const vector<Hit>& bm25_results,
                           const vector<Hit>& semantic_results,
                           int topk) {
        map<uint32_t, double> fused_scores;

        // Add BM25 scores with RRF
        for (int rank = 0; rank < bm25_results.size(); rank++) {
            uint32_t doc_id = bm25_results[rank].doc_id;
            fused_scores[doc_id] += 1.0 / (60 + rank + 1); // k=60
        }

        // Add semantic scores with RRF
        for (int rank = 0; rank < semantic_results.size(); rank++) {
            uint32_t doc_id = semantic_results[rank].doc_id;
            fused_scores[doc_id] += 1.0 / (60 + rank + 1); // k=60
        }

        // Sort by fused score and return top-k
        vector<pair<uint32_t, double>> sorted(fused_scores.begin(),
                                             fused_scores.end());
        sort(sorted.begin(), sorted.end(),
             [](auto& a, auto& b) { return a.second > b.second; });

        vector<Hit> results;
        for (int i = 0; i < std::min(topk, (int)sorted.size()); i++) {
            results.push_back({sorted[i].first, sorted[i].second});
        }
        return results;
    }

    shared_ptr<EmbeddingModel> embed_model_;
    vector<vector<float>> embeddings_;
};
```

**Benefits:**
- +5-15% recall improvement
- Catches semantic matches BM25 misses
- RRF balances both retrieval methods

**Effort:** 3-5 days
**Risk:** Medium (embedding model integration)
**Impact:** Semantic coverage

---

#### 3.4 Semantic Chunking
**Issues:** Fixed-size chunks may split semantic units

**Solution:** Use embedding similarity for boundaries

**Implementation:**
```python
# semantic_chunking.py (integrate into ocean_build.cpp)
import numpy as np
from sentence_transformers import SentenceTransformer

model = SentenceTransformer('all-MiniLM-L6-v2')

def semantic_chunk(text, target_tokens=2000):
    sentences = split_sentences(text)
    embeddings = model.encode(sentences)

    chunks = []
    current_chunk = []
    current_tokens = 0

    for i, (sentence, sent_emb) in enumerate(zip(sentences, embeddings)):
        sentence_tokens = count_tokens(sentence)

        if current_tokens + sentence_tokens > target_tokens:
            # Check if we should start a new chunk
            if current_chunk:
                prev_emb = np.mean(embeddings[i-len(current_chunk):i], axis=0)
                similarity = cosine_similarity(prev_emb, sent_emb)

                if similarity < 0.7:  # Threshold for topic shift
                    # Topic shift - end current chunk
                    chunks.append(' '.join(current_chunk))
                    current_chunk = [sentence]
                    current_tokens = sentence_tokens
                    continue

        current_chunk.append(sentence)
        current_tokens += sentence_tokens

    if current_chunk:
        chunks.append(' '.join(current_chunk))

    return chunks
```

**Benefits:**
- Better chunk boundaries
- Less noise in retrieved context
- +2-5% retrieval precision

**Effort:** 2-3 days
**Risk:** Medium (requires Python integration or C++ embedding model)
**Impact:** Chunk quality

---

### Phase 4: Index Management (1 week) 🟡

**Goal:** Enable incremental index updates

#### 4.1 Incremental Index Updates
**Issues:** Adding content requires full rebuild

**Solution:** Append-only manifest with periodic compaction

**Implementation:**
```cpp
// corpus_manager.hpp
class IncrementalIndex {
public:
    bool append_document(const string& content, const DocMeta& meta) {
        // Chunk the new document
        vector<ChunkIn> chunks = chunk_document(content);

        // Add to storage
        for (const auto& chunk : chunks) {
            string compressed = compress(chunk.text);
            uint64_t offset = append_to_storage(compressed);

            // Add to manifest
            DocMeta doc_meta = create_doc_meta(chunk, meta, offset);
            append_to_manifest(doc_meta);

            // Update inverted index
            update_inverted_index(doc_meta);
        }

        return true;
    }

    void compact_index() {
        // Periodically merge append-only manifest
        // Rebuild binary manifest with all documents
        // This can be done in background
    }

private:
    void append_to_manifest(const DocMeta& meta) {
        // Append to append-only manifest file
        std::ofstream out(manifest_path_append_, ios::app);
        out << serialize_doc_meta(meta) << endl;
    }

    void update_inverted_index(const DocMeta& meta) {
        // Update in-memory inverted index
        for (const string& kw : meta.keywords) {
            uint32_t doc_id = next_doc_id_++;
            inverted_index_[kw].push_back(doc_id);
        }
    }

    string manifest_path_append_ = "manifest_append.jsonl";
    string manifest_path_compacted_ = "manifest.bin";
    unordered_map<string, vector<uint32_t>> inverted_index_;
    uint32_t next_doc_id_ = 0;
};
```

**Benefits:**
- Add documents without full rebuild
- Fast indexing for small additions
- Background compaction maintains performance

**Effort:** 3-4 days
**Risk:** Medium (complexity)
**Impact:** Operational efficiency

---

### Phase 5: Testing & Documentation (1-2 weeks) 🟡

**Goal:** Comprehensive test coverage and documentation

#### 5.1 Expanded Test Suite
**Issues:** Only 10 test cases, no edge cases, no load testing

**Solution:** Comprehensive test framework

**Implementation:**
```cpp
// tests/test_search.cpp
#include <gtest/gtest.h>

class SearchTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Load test corpus
        corpus_ = load_test_corpus();
    }

    Corpus corpus_;
};

TEST_F(SearchTest, BasicQuery) {
    vector<Hit> results = search_bm25(corpus_, "test query", 10);
    EXPECT_GT(results.size(), 0);
    EXPECT_GT(results[0].score, 0);
}

TEST_F(SearchTest, EmptyQuery) {
    vector<Hit> results = search_bm25(corpus_, "", 10);
    EXPECT_EQ(results.size(), 0);
}

TEST_F(SearchTest, StopWordsOnly) {
    vector<Hit> results = search_bm25(corpus_, "the and for", 10);
    EXPECT_EQ(results.size(), 0);
}

TEST_F(SearchTest, StemmingWorks) {
    auto r1 = search_bm25(corpus_, "running", 10);
    auto r2 = search_bm25(corpus_, "run", 10);
    EXPECT_EQ(r1[0].doc_id, r2[0].doc_id); // Same doc
}

TEST_F(SearchTest, LongQuery) {
    string long_query(10000, 'a');  // 10K chars
    vector<Hit> results = search_bm25(corpus_, long_query, 10);
    // Should not crash, should return results
    EXPECT_GT(results.size(), 0);
}

TEST_F(SearchTest, SpecialCharacters) {
    vector<Hit> results = search_bm25(corpus_, "test@#$%^&*()", 10);
    // Should handle gracefully
    EXPECT_GE(results.size(), 0);
}

// Load testing
TEST(LoadTest, ConcurrentRequests) {
    Corpus corpus = load_test_corpus();
    vector<thread> threads;
    atomic<int> success_count = 0;
    atomic<int> failure_count = 0;

    for (int i = 0; i < 100; i++) {
        threads.emplace_back([&corpus, &success_count, &failure_count]() {
            try {
                auto results = search_bm25(corpus, "test query", 10);
                if (results.size() > 0) {
                    success_count++;
                } else {
                    failure_count++;
                }
            } catch (...) {
                failure_count++;
            }
        });
    }

    for (auto& t : threads) t.join();

    cout << "Success: " << success_count << ", Failures: " << failure_count << endl;
    EXPECT_GT(success_count, 90); // 90%+ success rate
}

// Adversarial inputs
TEST(AdversarialTest, SQLInjection) {
    Corpus corpus = load_test_corpus();
    vector<Hit> results = search_bm25(corpus_,
        "'; DROP TABLE chunks; --", 10);
    EXPECT_GE(results.size(), 0);
}

TEST(AdversarialTest, PathTraversal) {
    Corpus corpus = load_test_corpus();
    vector<Hit> results = search_bm25(corpus_,
        "../../../etc/passwd", 10);
    EXPECT_GE(results.size(), 0);
}

// Accuracy tests
TEST(AccuracyTest, TestSetAccuracy) {
    Corpus corpus = load_test_corpus();

    struct TestCase {
        string query;
        string expected_chunk_id;
    };

    vector<TestCase> test_cases = {
        {"Tony Balay", "guten9m_DOC_12345"},
        {"BBQ class cost", "guten9m_DOC_67890"},
        {"Carbon Copy Cloner", "guten9m_DOC_24680"}
        // ... all 10 test cases ...
    };

    int correct = 0;
    for (const auto& test : test_cases) {
        auto results = search_bm25(corpus, test.query, 1);
        if (results[0].doc_id == test.expected_chunk_id) {
            correct++;
        }
    }

    float accuracy = (float)correct / test_cases.size();
    cout << "Accuracy: " << (accuracy * 100) << "%" << endl;
    EXPECT_GE(accuracy, 0.95); // 95%+ accuracy
}
```

**Run tests:**
```bash
# Build with tests
g++ -std=c++17 -o tests -I../include tests/*.cpp ../src/*.cpp -lgtest -lgtest_main -lpthread

# Run all tests
./tests

# Run specific test
./tests --gtest_filter=SearchTest.StemmingWorks

# Run with coverage
./tests --gtest_output=xml:results.xml
```

**Benefits:**
- Comprehensive coverage
- Catches regressions
- Validates performance under load

**Effort:** 3-5 days
**Risk:** Low
**Impact:** Quality assurance

---

#### 5.2 Documentation
**Issues:** Limited production documentation

**Solution:** Complete documentation suite

**Structure:**
```
docs/
├── README.md                     # Quick start
├── ARCHITECTURE.md              # System design
├── API.md                       # REST API reference
├── CONFIGURATION.md             # Config options
├── DEPLOYMENT.md               # Production deployment
├── PERFORMANCE.md              # Benchmarks and optimization
├── TESTING.md                 # Test framework
├── TROUBLESHOOTING.md        # Common issues
└── CONTRIBUTING.md            # Dev guide
```

**Example: API.md**
```markdown
# OceanEterna REST API

## Endpoints

### POST /chat
Chat with the RAG system.

**Request:**
```json
{
  "question": "What is BBQ class cost?",
  "conversation_id": "optional-cid"
}
```

**Response:**
```json
{
  "answer": "The BBQ class costs $35.",
  "sources": [
    {"chunk_id": "guten9m_DOC_67890", "score": 2.5}
  ],
  "search_time_ms": 45.2,
  "llm_time_ms": 1234.5,
  "turn_id": "turn-12345"
}
```

### POST /tell-me-more
Ask follow-up question without re-searching.

**Request:**
```json
{
  "prev_turn_id": "turn-12345",
  "aspect": "more details"
}
```

### GET /health
Health check endpoint.

**Response:**
```json
{
  "status": "healthy",
  "uptime_seconds": 12345,
  "chunks_loaded": 5065226,
  "total_tokens": 1234567890
}
```
```

**Benefits:**
- Easier onboarding
- Clear API contract
- Production deployment guide

**Effort:** 2-3 days
**Risk:** None
**Impact:** Usability

---

## Unified Implementation Timeline

### Phase 1: Critical Production Readiness (Weeks 1-2)
- [ ] HTTP server refactor (cpp-httplib) - 2-3 days
- [ ] Code modularization - 2-3 days
- [ ] Configuration externalization - 1-2 days
- [ ] LLM error handling & retry logic - 2-3 days
- [ ] Production basics (auth, rate limiting, logging) - 3-4 days

**Total: 10-15 days**

---

### Phase 2: Performance Optimization (Weeks 3-4)
- [ ] Memory-mapped manifests - 1-2 days
- [ ] Zstd compression - 2-3 hours
- [ ] BlockWeakAnd early termination - 2-3 days
- [ ] SIMD vectorization - 1-2 days

**Total: 5-9 days**

---

### Phase 3: Advanced Search (Weeks 5-7)
- [ ] Porter stemmer - 1-2 days
- [ ] Improved keyword extraction - 1-2 days
- [ ] Hybrid search with RRF - 3-5 days
- [ ] Semantic chunking - 2-3 days

**Total: 7-12 days**

---

### Phase 4: Index Management (Week 8)
- [ ] Incremental index updates - 3-4 days
- [ ] Background compaction - 2-3 days

**Total: 5-7 days**

---

### Phase 5: Testing & Documentation (Weeks 9-10)
- [ ] Expanded test suite - 3-5 days
- [ ] Load testing - 2-3 days
- [ ] Complete documentation - 2-3 days

**Total: 7-11 days`

---

## Projected Final Metrics

### Performance
| Metric | Current | After All Phases | Improvement |
|--------|---------|------------------|-------------|
| Startup Time | 13s | 5-8s | 2x faster |
| Search Latency | 700ms | 50-150ms | 5-14x faster |
| Storage Size | 8.3 GB | 5.5-6 GB | 33% smaller |
| RAM Usage | 8 GB | 1.6-3 GB | 60-80% reduction |

### Quality
| Metric | Current | After All Phases | Improvement |
|--------|---------|------------------|-------------|
| Recall | 100% (lexical) | 105-115% (hybrid) | +5-15% |
| Test Coverage | 10 cases | 100+ cases | 10x more |
| Production Ready | 4/10 | 9/10 | +125% |

### Production Capabilities
| Feature | Current | After All Phases |
|---------|---------|------------------|
| HTTP Library | Raw sockets | cpp-httplib |
| Error Handling | None | Retry + backoff |
| Authentication | None | API key based |
| Rate Limiting | None | Configurable |
| Request Logging | None | JSON logs |
| Graceful Shutdown | None | Signal handlers |
| Incremental Updates | None | Yes |
| Stemming | None | Porter stemmer |
| Semantic Search | No | Hybrid (RRF) |
| Tests | 10 cases | 100+ cases |

---

## Risk Assessment

### High Risk
- **BlockWeakAnd implementation** - Algorithm complexity
- **Hybrid search** - Embedding model integration
- **Incremental updates** - Data consistency

### Medium Risk
- **Code modularization** - Refactoring breaks
- **Error handling** - Complex retry logic
- **Semantic chunking** - Python/C++ integration

### Low Risk
- **HTTP library refactor** - Well-tested library
- **Config externalization** - Simple changes
- **Memory mapping** - Standard technology
- **Zstd compression** - Drop-in replacement
- **SIMD vectorization** - Runtime detection

---

## Comparison with Commercial Solutions

### Before Improvements
| Feature | OceanEterna | Weaviate | Qdrant |
|---------|-------------|-----------|---------|
| Startup | 13s | 20-60s | 15-40s |
| Search | 700ms | 50-200ms | 10-50ms |
| RAM | 8 GB | 15-25 GB | 12-20 GB |
| Production Ready | 4/10 | 9/10 | 9/10 |

### After Improvements
| Feature | OceanEterna | Weaviate | Qdrant |
|---------|-------------|-----------|---------|
| Startup | 5-8s | 20-60s | 15-40s |
| Search | 50-150ms | 50-200ms | 10-50ms |
| RAM | 1.6-3 GB | 15-25 GB | 12-20 GB |
| Production Ready | 9/10 | 9/10 | 9/10 |

**Key Insight:** After improvements, OceanEterna is **competitive** with commercial solutions on performance and production-readiness, while maintaining advantages:
- 3-10x less RAM usage
- 2-3x faster startup
- Zero costs
- 100% local (no cloud)
- Better conversation features

---

## Conclusion

This unified plan combines insights from both analyses to provide a **comprehensive roadmap** for transforming OceanEterna:

### What We're Fixing

**From Claude's Analysis (Production Readiness):**
- ✅ HTTP server robustness (raw sockets → cpp-httplib)
- ✅ Code structure (monolithic → modular)
- ✅ Configuration (hardcoded → external)
- ✅ Error handling (none → retry + backoff)
- ✅ Production basics (auth, rate limiting, logging)

**From OpenCode's Analysis (Performance):**
- ✅ Search speed (700ms → 50-150ms)
- ✅ Memory usage (8GB → 1.6-3GB)
- ✅ Storage efficiency (LZ4 → Zstd)
- ✅ Semantic coverage (BM25 → hybrid)

**New Insights:**
- ✅ Incremental index updates
- ✅ Comprehensive testing
- ✅ Complete documentation

### Timeline & Effort

- **Phase 1 (Production):** 10-15 days
- **Phase 2 (Performance):** 5-9 days
- **Phase 3 (Search):** 7-12 days
- **Phase 4 (Index):** 5-7 days
- **Phase 5 (Testing):** 7-11 days

**Total: 5-7 weeks for full transformation**

### Expected Outcome

After completing all phases, OceanEterna will be:
- **9/10 production-ready** (vs 4/10 today)
- **Competitive with commercial solutions** on speed
- **3-10x more memory efficient** than commercial
- **More feature-rich** (conversation caching, semantic search)
- **Zero-cost** (no licensing or per-query fees)

### Recommendation

**Start with Phase 1 immediately** - These are critical for production deployment. The HTTP server refactor alone removes a major risk factor.

**Then proceed sequentially** through Phases 2-5, testing each phase before moving to the next.

**Total investment:** 5-7 weeks
**Return value:** Production-ready, high-performance RAG system with zero ongoing costs.

---

**Document Version:** 1.0
**Last Updated:** February 1, 2026
**Next Review:** After Phase 1 completion
