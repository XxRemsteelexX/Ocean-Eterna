# OceanEterna - Quick Reference: Unified Plan

**Date:** February 1, 2026
**Based on:** OpenCode + Claude (Anthropic) analysis

---

## Executive Summary

**Current State:** 6/10 - Solid research project, needs production hardening

**After All Improvements:** 9/10 - Production-ready, competitive with commercial

**Timeline:** 5-7 weeks
**Total Effort:** 34-54 days

---

## What Claude Identified (Production Issues)

| # | Issue | Priority |
|---|-------|----------|
| 1 | HTTP server - Raw sockets risky | 🔴 High |
| 2 | 2200 lines in one file | 🔴 High |
| 3 | Hardcoded config (API URL, model) | 🔴 High |
| 4 | BM25 alone (no stemming) | 🔴 High |
| 5 | Basic keyword extraction | 🟡 Medium |
| 6 | No incremental index updates | 🟡 Medium |
| 7 | No LLM error handling/retry | 🔴 High |
| 8 | Memory constraints (3GB index) | 🔴 High |
| 9 | Limited test coverage (10 cases) | 🟡 Medium |
| 10 | No production basics (auth, rate limiting) | 🔴 High |

---

## What OpenCode Identified (Performance Issues)

| # | Issue | Priority |
|---|-------|----------|
| 1 | Search speed: 700ms | 🔴 High |
| 2 | Memory usage: 8GB | 🔴 High |
| 3 | Debug logging in production | 🟢 Low |
| 4 | Hardcoded API key | 🔴 High |
| 5 | Large pattern matching list | 🟢 Low |
| 6 | Compression: LZ4 (2.68x) | 🟡 Medium |
| 7 | No semantic search | 🟡 Medium |
| 8 | No SIMD vectorization | 🟡 Medium |
| 9 | Large file size (1400+ lines) | 🟡 Medium |

---

## Combined Top 10 Priorities

### 🔴 Critical (Weeks 1-2)

#### 1. HTTP Server Refactor
**Why:** Raw sockets are fragile, edge cases, security risk

**Solution:** Use cpp-httplib (header-only, ~3000 lines)

**Code Example:**
```cpp
#include <httplib.h>

httplib::Server svr;

svr.Post("/chat", [](const httplib::Request& req, httplib::Response& res) {
    // Automatic Content-Length parsing
    // Built-in CORS handling
    json body = json::parse(req.body);
    res.set_content(handle_chat(body).dump(), "application/json");
});

svr.listen("0.0.0.0", 8888);
```

**Effort:** 2-3 days | **Impact:** Removes critical risk

---

#### 2. Code Modularization
**Why:** 2200 lines in one file, hard to test

**Solution:** Split into 8 modules
```
ocean_chat_server.cpp → 100 lines (main only)
http_server.hpp/cpp → 300 lines
search_engine.hpp/cpp → 400 lines
llm_client.hpp/cpp → 200 lines
conversation.hpp/cpp → 300 lines
corpus_manager.hpp/cpp → 200 lines
config.hpp/cpp → 100 lines
utils.hpp/cpp → 150 lines
```

**Effort:** 2-3 days | **Impact:** Maintainability

---

#### 3. Configuration Externalization
**Why:** Hardcoded API URL, model, API key

**Solution:** JSON config + environment variables

**config.json:**
```json
{
  "server": {"port": 8888},
  "llm": {
    "api_url": "https://routellm.abacus.ai/v1/chat/completions",
    "model": "gpt-5-mini",
    "api_key_env": "OCEAN_ETERNA_API_KEY"
  },
  "search": {"top_k": 8}
}
```

**Effort:** 1-2 days | **Impact:** Deployment flexibility

---

#### 4. LLM Error Handling & Retry
**Why:** No retry logic, no exponential backoff

**Solution:** Robust error handling

```cpp
Response query_with_retry(const string& prompt, int max_retries = 3) {
    for (int attempt = 0; attempt < max_retries; attempt++) {
        Response resp = send_request(prompt);

        if (resp.success) return resp;

        if (is_retryable_error(resp.error)) {
            // Exponential backoff
            int delay = 1000 * (1 << attempt); // 1s, 2s, 4s
            this_thread::sleep_for(chrono::milliseconds(delay));
        }
    }
    return {false, "", 0, "Max retries exceeded"};
}
```

**Plus Circuit Breaker:**
- Prevents cascading failures
- Fallback to local LLM option

**Effort:** 2-3 days | **Impact:** Production reliability

---

#### 5. Production Basics
**Why:** No auth, rate limiting, logging, graceful shutdown

**Solution:** Add production infrastructure

**Authentication:**
```cpp
svr.set_pre_routing_handler([&config](const httplib::Request& req, httplib::Response& res) {
    auto it = req.headers.find("X-API-Key");
    if (it == req.headers.end() || it->second != config.auth.api_key) {
        res.status = 401;
        res.set_content("Unauthorized", "text/plain");
        return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
});
```

**Rate Limiting:**
```cpp
class RateLimiter {
    bool is_allowed(const string& ip) {
        auto& bucket = buckets_[ip];
        auto now = chrono::system_clock::now();

        // Clean old entries (1 min ago)
        bucket.erase(remove_if(bucket.begin(), bucket.end(),
            [now](auto ts) { return now - ts > 1min; }), bucket.end());

        if (bucket.size() >= 60) return false; // 60 req/min

        bucket.push_back(now);
        return true;
    }
};
```

**Effort:** 3-4 days | **Impact:** Production deployment

---

### 🟡 High Priority (Weeks 3-4)

#### 6. Memory-Mapped Manifests
**Why:** 8 GB RAM loaded at startup

**Solution:** Use mmap for on-demand loading

```cpp
void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);

// OS handles paging, only loaded chunks in RAM
DocMeta chunk = read_chunk(mapped, offset);
```

**Results:** 8GB → 1.6-3GB RAM (60-80% reduction)

**Effort:** 1-2 days | **Impact:** Memory efficiency

---

#### 7. BlockWeakAnd Early Termination
**Why:** Scores all 5M docs, even when only top-8 needed

**Solution:** Skip unpromising blocks

```
For each block:
  Calculate max possible score
  If max < threshold: SKIP (don't score this block)
  Else: Score only promising blocks
```

**Results:** 700ms → 140-350ms (2-5x faster)

**Effort:** 2-3 days | **Impact:** Search speed

---

#### 8. Zstd Compression
**Why:** LZ4 has 2.68x ratio, could be better

**Solution:** Switch to Zstd-3 (4.0x ratio)

```cpp
size_t compressed = ZSTD_compress(dst, cap, src, size, 3);
size_t decompressed = ZSTD_decompress(dst, cap, src, comp_size);
```

**Results:** 10.2GB → 6.8GB (33% smaller)
**Query impact:** 0.8ms → 2.5ms (negligible)

**Effort:** 2-3 hours | **Impact:** Storage efficiency

---

#### 9. SIMD Vectorization
**Why:** No AVX/AVX-512 for vector operations

**Solution:** Add SIMD for TF-IDF calculations

```cpp
// AVX2: Process 8 floats at once
__m256 sum = _mm256_setzero_ps();
for (int i = 0; i + 8 <= n; i += 8) {
    __m256 va = _mm256_load_ps(&a[i]);
    __m256 vb = _mm256_load_ps(&b[i]);
    sum = _mm256_add_ps(sum, _mm256_mul_ps(va, vb));
}

// AVX-512: 16 floats at once (even faster)
```

**Results:** 2-4x speedup on vectorizable code

**Effort:** 1-2 days | **Impact:** Search speed

---

### 🟡 Medium Priority (Weeks 5-7)

#### 10. Porter Stemmer
**Why:** No stemming, "ocean mammals" ≠ "ocean mammal"

**Solution:** Add Porter stemmer (~200 lines)

```cpp
string stem(const string& word) {
    string w = word;
    w = step1ab(w);  // Remove plurals
    w = step1c(w);  // Handle -y endings
    w = step2(w);    // Remove suffixes
    // ... Porter rules ...
    return w;
}

// Use in search
vector<string> terms = {"ocean", "mammals"};
for (auto& term : terms) term = stem(term);
// Now: "ocean", "mammal" (both stem to same)
```

**Results:** +5-10% recall improvement

**Effort:** 1-2 days | **Impact:** Recall accuracy

---

## Additional Improvements (Optional)

### Hybrid Search with RRF
- **Benefit:** +5-15% recall
- **Effort:** 3-5 days
- **Priority:** 🟡 Medium

### Improved Keyword Extraction
- **Benefit:** Capture 2-char terms (AI, ML, US)
- **Effort:** 1-2 days
- **Priority:** 🟡 Medium

### Semantic Chunking
- **Benefit:** Better chunk boundaries
- **Effort:** 2-3 days
- **Priority:** 🟡 Medium

### Incremental Index Updates
- **Benefit:** Add docs without full rebuild
- **Effort:** 3-4 days
- **Priority:** 🟡 Medium

### Comprehensive Testing
- **Benefit:** 100+ test cases, load testing
- **Effort:** 3-5 days
- **Priority:** 🟡 Medium

---

## Implementation Timeline

### Week 1-2: Critical Production Readiness 🔴
- [ ] HTTP server refactor (cpp-httplib) - 2-3 days
- [ ] Code modularization - 2-3 days
- [ ] Configuration externalization - 1-2 days
- [ ] LLM error handling & retry - 2-3 days
- [ ] Production basics (auth, rate limiting) - 3-4 days

**Deliverable:** Production-ready HTTP server

---

### Week 3-4: Performance Optimization 🟡
- [ ] Memory-mapped manifests - 1-2 days
- [ ] Zstd compression - 2-3 hours
- [ ] BlockWeakAnd early termination - 2-3 days
- [ ] SIMD vectorization - 1-2 days

**Deliverable:** 5-14x faster search, 60-80% less RAM

---

### Week 5-7: Advanced Search & Testing 🟡
- [ ] Porter stemmer - 1-2 days
- [ ] Hybrid search with RRF - 3-5 days
- [ ] Semantic chunking - 2-3 days
- [ ] Incremental index updates - 3-4 days
- [ ] Comprehensive testing - 3-5 days

**Deliverable:** +5-15% recall, complete test coverage

---

## Expected Outcomes

### Performance Metrics

| Metric | Current | After All Phases |
|--------|---------|------------------|
| Startup Time | 13s | 5-8s (2x faster) |
| Search Latency | 700ms | 50-150ms (5-14x faster) |
| Storage Size | 8.3 GB | 5.5-6 GB (33% smaller) |
| RAM Usage | 8 GB | 1.6-3 GB (60-80% reduction) |

### Quality Metrics

| Metric | Current | After All Phases |
|--------|---------|------------------|
| Recall | 100% (lexical) | 105-115% (hybrid) |
| Test Coverage | 10 cases | 100+ cases |
| Production Ready | 4/10 | 9/10 |

### vs Commercial Solutions

| Feature | OceanEterna (After) | Weaviate | Qdrant |
|---------|---------------------|-----------|---------|
| Search | 50-150ms | 50-200ms | 10-50ms |
| RAM | 1.6-3 GB | 15-25 GB | 12-20 GB |
| Startup | 5-8s | 20-60s | 15-40s |
| Production Ready | 9/10 | 9/10 | 9/10 |

**Key Insight:** Competitive with commercial, 3-10x less RAM, zero costs

---

## Quick Wins (First 24 Hours)

1. **Remove debug logging** (1 hour)
   ```cpp
   #ifdef DEBUG_MODE
   cout << "===== RAW RESPONSE =====" << endl;
   cout << response_string.substr(0, 500) << endl;
   #endif
   ```

2. **Move API key to env var** (30 min)
   ```cpp
   const string API_KEY = []() {
       const char* env = getenv("OCEAN_ETERNA_API_KEY");
       return env ? string(env) : "";
   }();
   ```

3. **Install cpp-httplib** (5 min)
   ```bash
   wget https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
   ```

4. **Create config.json** (30 min)
   ```json
   {
     "llm": {"api_key_env": "OCEAN_ETERNA_API_KEY"},
     "search": {"top_k": 8}
   }
   ```

**Total Time:** 2 hours
**Immediate Impact:** Cleaner code, security, flexibility

---

## Risk Assessment

### High Risk Items
- BlockWeakAnd implementation (algorithm complexity)
- Hybrid search (embedding model integration)
- Incremental updates (data consistency)

### Mitigation Strategies
- Extensive testing before production
- Feature flags (can disable new features)
- Rollback plan (keep old code in version control)
- A/B testing (compare old vs new)

---

## Key Files to Modify

| Phase | File | Changes |
|-------|------|---------|
| 1 | ocean_chat_server.cpp | Split into modules |
| 1 | ocean_chat_server.cpp | Replace HTTP with cpp-httplib |
| 1 | ocean_chat_server.cpp | Add config loader |
| 1 | llm_client.cpp | Add retry logic |
| 1 | ocean_chat_server.cpp | Add auth, rate limiting |
| 2 | binary_manifest.hpp | Add mmap support |
| 2 | ocean_build.cpp | Add Zstd compression |
| 2 | search_engine.cpp | Add BlockWeakAnd |
| 2 | utils.cpp | Add SIMD vectorization |
| 3 | search_engine.cpp | Add Porter stemmer |
| 3 | hybrid_search.cpp | New file for RRF fusion |
| 3 | ocean_build.cpp | Add semantic chunking |
| 4 | corpus_manager.cpp | Add incremental updates |
| 5 | tests/ | Add comprehensive tests |
| 5 | docs/ | Add complete documentation |

---

## Dependencies

### New Libraries Needed

```bash
# HTTP server (replaces raw sockets)
# Header-only, no installation needed
wget https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h

# Zstd compression
sudo apt install libzstd-dev  # Ubuntu/Debian
brew install zstd             # macOS

# Google Test framework
sudo apt install libgtest-dev  # Ubuntu/Debian
brew install googletest       # macOS

# (Optional) Embedding models for hybrid search
pip install sentence-transformers
```

---

## Build System Updates

### CMakeLists.txt (Recommended)

```cmake
cmake_minimum_required(VERSION 3.15)
project(OceanEterna)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(Zstd REQUIRED)
find_package(GTest REQUIRED)
find_package(OpenMP REQUIRED)

# Sources
set(SOURCES
    src/http_server.cpp
    src/search_engine.cpp
    src/llm_client.cpp
    src/conversation.cpp
    src/corpus_manager.cpp
    src/config.cpp
    src/utils.cpp
    src/ocean_chat_server.cpp
)

# Build main binary
add_executable(ocean_chat_server ${SOURCES})
target_link_libraries(ocean_chat_server
    PRIVATE
    Zstd::Zstd
    OpenMP::OpenMP_CXX
    lz4
    curl
    pthread
)

# Build tests
enable_testing()
add_executable(tests tests/test_search.cpp tests/test_api.cpp)
target_link_libraries(tests
    PRIVATE
    GTest::gtest
    GTest::gtest_main
    ocean_chat_server  # Link to main code
)
add_test(NAME AllTests COMMAND tests)
```

---

## Testing Strategy

### Unit Tests (GTest)
```bash
# Build tests
cmake -B build -S .
cmake --build build --target tests

# Run all tests
./build/tests

# Run specific test
./build/tests --gtest_filter=SearchTest.StemmingWorks
```

### Integration Tests
```bash
# Test real API endpoints
curl -X POST http://localhost:8888/chat \
  -H "Content-Type: application/json" \
  -H "X-API-Key: test-key" \
  -d '{"question":"test"}'
```

### Load Testing
```bash
# 100 concurrent requests
for i in {1..100}; do
  curl -X POST http://localhost:8888/chat \
    -H "Content-Type: application/json" \
    -d '{"question":"test"}' &
done
wait
```

---

## Documentation Structure

```
docs/
├── README.md              # Quick start guide
├── ARCHITECTURE.md       # System design
├── API.md                # REST API reference
├── CONFIGURATION.md      # Config options
├── DEPLOYMENT.md        # Production deployment
├── PERFORMANCE.md       # Benchmarks
├── TESTING.md          # Test framework
├── TROUBLESHOOTING.md  # Common issues
└── CONTRIBUTING.md     # Dev guide
```

---

## Success Criteria

### Phase 1 (Week 2)
- ✅ HTTP server uses cpp-httplib
- ✅ Code split into 8+ modules
- ✅ Config externalized (JSON + env vars)
- ✅ LLM retry logic with exponential backoff
- ✅ Auth, rate limiting, request logging working

### Phase 2 (Week 4)
- ✅ Search latency: 50-150ms
- ✅ RAM usage: 1.6-3 GB
- ✅ Storage: 5.5-6 GB
- ✅ All tests pass

### Phase 3 (Week 7)
- ✅ Recall: 105-115% (hybrid)
- ✅ Test coverage: 100+ cases
- ✅ Incremental updates working
- ✅ Documentation complete

---

## Conclusion

**Claude's analysis focused on:** Production readiness, robustness, error handling
**OpenCode's analysis focused on:** Performance, speed, memory efficiency

**Unified plan addresses:**
- ✅ Both sets of concerns
- ✅ Priority order (critical → high → medium)
- ✅ Realistic timeline (5-7 weeks)
- ✅ Measurable success criteria

**Expected outcome:**
- Production-ready system (9/10)
- Competitive with commercial solutions
- 3-10x more memory efficient
- Zero ongoing costs
- Better conversation features

**Start with Phase 1 immediately** - HTTP server refactor removes critical production risk.

**Document:** Full details in `UNIFIED_IMPROVEMENT_PLAN.md`
