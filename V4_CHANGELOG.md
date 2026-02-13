# OceanEterna v4 Changelog

## Step 26: Frontend SSE Support (Feb 5, 2026)
- **Updated ocean_demo.html to use streaming /chat/stream endpoint**
- New functions for progressive UI updates:
  - `addStreamingMessage()` - Creates message element for streaming
  - `appendToStreamingMessage()` - Appends tokens as they arrive
  - `finalizeStreamingMessage()` - Adds metadata, sources after stream ends
- Streaming UX features:
  - Blinking cursor animation (▋) during streaming
  - Search metadata shown immediately when available
  - Tokens appear progressively in message
  - Sources and timing badges added on completion
- Fallback to regular `/chat` endpoint if streaming fails
- Original `sendMessage()` split into streaming-aware version + `sendMessageFallback()`
- CSS: Added `.streaming-text::after` with blink animation
- **Accuracy: 10/10 (100%) PASSED** (backend unchanged)
- Backup: backups/ocean_demo_pre_step26.html

## Step 25: SSE Streaming Endpoint /chat/stream (Feb 5, 2026)
- **Added POST /chat/stream endpoint for Server-Sent Events streaming**
- Same search logic as /chat (BM25, context building, conversation saving)
- SSE event format:
  - `event: search` - Immediate search metadata (chunks_retrieved, search_time_ms)
  - `data: {"token": "..."}` - Individual LLM tokens as they stream
  - `event: done` - Final metadata (turn_id, total_time_ms, sources)
- SSE headers: `Content-Type: text/event-stream`, `Cache-Control: no-cache`
- Saves conversation turn to database (same as /chat)
- Updates chapter guide and stats
- Token streaming depends on LLM API supporting OpenAI-compatible streaming
- Existing /chat endpoint unchanged for backward compatibility
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step25.cpp

## Step 24: Streaming LLM Client (Feb 5, 2026)
- **Added `query_llm_streaming()` function to llm_client.hpp**
- Enables SSE (Server-Sent Events) streaming from LLM API
- Uses `"stream": true` in request JSON
- CURL callback parses SSE format: `data: {"choices": [{"delta": {"content": "token"}}]}`
- Calls user-provided `std::function<void(const string&)>` callback for each token
- Returns complete assembled response when stream ends
- Handles `data: [DONE]` stream termination signal
- Added `StreamingContext` struct for callback state management
- **Accuracy: 10/10 (100%) PASSED** (existing non-streaming path unchanged)
- Compile: SUCCESS | Backup: backups/llm_client.hpp.step23

## Step 23: Update BM25 Formula to Use TF (Feb 5, 2026)
- **Updated search_bm25() to use proper BM25 with term frequency**
- Old formula: `score = idf * (k1 + 1.0) / (1.0 + norm)` (assumed tf=1)
- New formula: `score = idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * dl/avgdl))`
- Looks up tf from `corpus.tf_index` for new documents
- Falls back to tf=1 for legacy 5M chunks (produces identical scores)
- Documents with repeated keywords now score higher than single-mention docs
- **Accuracy: 10/10 (100%) PASSED** (legacy docs unaffected since tf=1 => same formula)
- Compile: SUCCESS | Backup: search_engine_pre_step23.hpp

## Step 22: Add TF Index for New Documents (Feb 5, 2026)
- **Added term frequency tracking for BM25 improvement**
- Added `tf_index` to Corpus struct: `unordered_map<string, unordered_map<uint32_t, uint16_t>>`
  - Maps keyword -> (doc_id -> term_frequency)
  - Only populated for new documents; legacy 5M chunks assumed tf=1
- New functions that return keywords with frequencies:
  - `extract_text_keywords_with_tf()` in search_engine.hpp
  - `generate_chat_keywords_with_tf()` in ocean_chat_server.cpp
- Updated `save_conversation_turn()` to populate tf_index for new conversations
- Updated `add_file_to_index()` to populate tf_index for uploaded files
- Added `tf_map` field to ChunkData struct
- **Accuracy: 10/10 (100%) PASSED** (existing search unchanged)
- Compile: SUCCESS | Backup: ocean_chat_server_pre_step22.cpp, search_engine_pre_step22.hpp

## Step 21: Fix Test Suite False Passes (Feb 5, 2026)
- **Fixed /query/* tests that were sending wrong field names**
- Old tests sent `{"question": "..."}` but endpoints expected different fields
- The `catch(...)` blocks returned 200 with error JSON, so status-only checks passed
- Fixed tests:
  - `/query/code-discussions`: now sends `{"chunk_id": "guten9m.1"}`, asserts response has `chunk_id`
  - `/query/fixes`: now sends `{"filename": "test.cpp"}`, asserts response has `filename`
  - `/query/feature`: now sends `{"feature_id": "feature1"}`, asserts response has `feature_id`
- **Comprehensive test: 50/50 passed (100%) - up from 47 (3 new assertions)**
- Backup: backups/test_comprehensive_pre_step21.py

## Step 20: Dead Code Cleanup (Feb 5, 2026)
- **Removed dead code and stale comments**
- Removed `compress_chunk_lz4()` from ocean_chat_server.cpp - was never called
- Removed stale comment "Simple HTTP server response" before RateLimiter class
- Removed `search_bm25_fast()` wrapper from search_engine.hpp - was just calling search_bm25()
- Already replaced all `search_bm25_fast()` calls with `search_bm25()` in Step 17
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step20.cpp, search_engine_pre_step20.hpp

## Step 19: LZ4 Buffer Safety (Feb 5, 2026)
- **Fixed potential buffer overflow in LZ4 decompression**
- Old code: allocated `length * 10` and assumed it was enough (silent overflow if ratio > 10:1)
- New code: iterative decompression with buffer growth
  - Start with 10x estimate
  - Consume input in chunks via `LZ4F_decompress()` in a loop
  - Grow buffer (double size) if output approaches capacity
  - 100 MB sanity limit to prevent runaway allocation
- Properly tracks `dst_pos` through iterations for correct return size
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step19.cpp

## Step 18: Fix Double Decompression (Feb 5, 2026)
- **Fixed double decompression in handle_chat() - reduces file I/O by 50%**
- Old code: loop 1 decompresses for context, loop 2 decompresses again for snippets (16 file opens per query)
- New code: single loop decompresses and caches in `decompressed_chunks` vector (8 file opens per query)
- ChunkReference building now uses cached content instead of re-decompressing
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step18.cpp

## Step 17: Thread Safety (Feb 5, 2026)
- **Fixed all thread safety bugs identified in v4_honest_review.md**
- Added `#include <shared_mutex>` for reader-writer locking
- Made `chat_chunk_counter` atomic (was plain `int`)
- Added `shared_mutex corpus_mutex` for corpus access:
  - `unique_lock` (exclusive) in `save_conversation_turn()`, `clear_conversation_database()`, `add_file_to_index()`
  - `shared_lock` (shared) in `get_chunk_by_id()`, `search_conversation_chunks_only_locked()`, `handle_chat()`
- Added `mutex stats_mutex` for stats updates in `handle_chat()` and `get_system_stats()`
- Added `mutex cache_mutex` for `recent_turns_cache` in `handle_source_query()`, `handle_tell_me_more()`, `handle_chat()`
- Refactored `handle_chat()` to extract doc metadata under lock before decompression
- Removed duplicate `chunk_id_to_index` update (now done inside `save_conversation_turn()`)
- **Accuracy: 10/10 (100%) PASSED**
- **Comprehensive test: 47/47 (100%) including 10-thread concurrent test**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step17.cpp

## Step 16: Final Documentation (Feb 5, 2026)
- Updated `README.md` for v4.0 (performance, quick start, endpoints, file structure)
- Created `ARCHITECTURE.md` (module diagram, data flow, memory layout, components)
- Created `API.md` (all 15 endpoints with request/response examples, CORS, auth, rate limiting)
- Created `CONFIGURATION.md` (config.json reference, env vars, all sections documented)
- `V4_CHANGELOG.md` maintained throughout all 17 steps (this file)

## Step 15: Comprehensive Test Suite (Feb 5, 2026)
- Created `test_comprehensive.py` with 47 tests across 7 sections:
  - Section 1: 10 accuracy tests (same as accuracy_test.py)
  - Section 2: 12 GET endpoint tests (/health, /stats, /guide, /chunk)
  - Section 3: 14 POST endpoint tests (/sources, /tell-me-more, /reconstruct, /extract-ids,
    /query/*, /add-file, /clear-database)
  - Section 4: 6 edge cases (empty, long, special chars, unicode, malformed JSON, missing fields)
  - Section 5: 2 performance tests (avg < 200ms, max < 500ms)
  - Section 6: 1 concurrent test (10 threads simultaneous)
  - Section 7: 2 CORS tests (OPTIONS preflight, Allow-Origin header)
- Auto-starts/stops server, cleans up test data from manifest
- **Result: 47/47 passed (100%)**
- Performance: avg search 12ms, max 42ms (10-100x faster than v3)

## Step 14: Zstd Compression Support (Feb 5, 2026)
- Added `#include <zstd.h>` and `-lzstd` linker flag
- New `compress_chunk()` uses Zstd level 3 (was LZ4)
- Kept `compress_chunk_lz4()` as fallback reference
- Updated `decompress_chunk()` with auto-format detection:
  - Reads first 4 bytes: LZ4 magic `04 22 4D 18` vs Zstd magic `28 B5 2F FD`
  - Routes to correct decompressor automatically
  - **All 5M+ existing LZ4 chunks decompress correctly**
- New chunks (file uploads, conversation saves) use Zstd
- Manifest entries updated to `"compression": "zstd"` for new data
- **Tested: Added zstd_test.txt, verified compression tag and successful decompression**
- Compile command: `g++ -O3 -std=c++17 -march=native -fopenmp -o ocean_chat_server_v4 ocean_chat_server.cpp -llz4 -lcurl -lpthread -lzstd`
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step14.cpp

## Step 13: Code Modularization (Feb 5, 2026)
- **Extracted 3 header-only modules from monolithic ocean_chat_server.cpp**
- `config.hpp` (~120 lines): Config struct, load_config(), get_env_or()
- `llm_client.hpp` (~155 lines): curl_write_cb, query_llm_once(), query_llm() with retry+backoff
- `search_engine.hpp` (~172 lines): ABBREV_WHITELIST, STOP_WORDS, extract_text_keywords(),
  build_stemmed_index(), get_stem(), search_bm25(), search_bm25_fast()
- Removed duplicate keyword extraction code from main .cpp after header extraction
- **Main .cpp: 2382 lines -> 1951 lines (18% reduction)**
- Single compile unit preserved (same g++ command works)
- Full directory backup: `chat_backup_step13/`
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step13.cpp

## Step 0: Create v4 Working Copy (Feb 5, 2026)
- Created `/home/yeblad/OE_1.24.26_v4/` as a copy of v3
- v3 preserved untouched as gold standard backup
- Updated accuracy_test.py to reference ocean_chat_server_v4
- All data files (13 GB) copied for isolation

## Step 1: Remove Debug Logging (Feb 5, 2026)
- Added `#ifdef DEBUG_MODE` macro guards (lines 33-41)
- Wrapped 15 debug log lines in DEBUG_LOG/DEBUG_ERR macros:
  - query_llm() raw response dump and parse debug (lines 472-486)
  - add_file_to_index() progress logging (lines 786-1042, 7 locations)
  - Query routing debug messages (lines 1571, 1599)
  - add-file-path body/JSON debug (lines 2038, 2041)
- No functional changes. Debug available via `#define DEBUG_MODE`
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step1.cpp

## Step 2: Move API Key to Environment Variable (Feb 5, 2026)
- Added `get_env_or()` helper function for env var reading with fallbacks
- OCEAN_API_KEY: API key (no hardcoded default -- must be set via env)
- OCEAN_API_URL: LLM endpoint URL (defaults to routellm.abacus.ai)
- OCEAN_MODEL: LLM model name (defaults to gpt-5-mini)
- Added startup warning if OCEAN_API_KEY is not set
- Updated version banner to v4.0
- **Security: No API keys in source code**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step2.cpp

## Step 3: Create config.json and Config Loader (Feb 5, 2026)
- Created `config.json` with server/llm/search/corpus sections
- Added `Config` struct with all configurable parameters
- Added `load_config()` function with JSON parsing + env var overrides
- Priority: env vars > config.json > defaults (hardcoded)
- Replaced hardcoded BM25 k1/b with `g_config.search.k1/b`
- All old constants (TOPK, HTTP_PORT, etc.) are now `#define` aliases to g_config
- chapter_guide_path now from config
- **No recompilation needed to change settings**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step3.cpp

## Step 4: Add LLM Retry with Exponential Backoff (Feb 5, 2026)
- Renamed `query_llm()` to `query_llm_once()` (single call, no retry)
- New `query_llm()` wraps with retry loop using `g_config.llm.max_retries`
- Exponential backoff: 1s, 2s, 4s (configurable via `retry_backoff_ms`)
- Retries on: timeout, rate limit, 502, 503, connection errors
- Non-retryable errors returned immediately
- Total elapsed time tracked across retries
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step4.cpp

## Step 5: Add Graceful Shutdown (Feb 5, 2026)
- Updated in Step 6 to use httplib::Server::stop() instead of raw socket close

## Step 6: Replace Raw Sockets with cpp-httplib (Feb 5, 2026)
- Downloaded cpp-httplib v0.18.3 (httplib.h, 344KB header-only)
- Removed ~400 lines of raw socket code (send_http_response + run_http_server)
- Replaced with cpp-httplib Server using lambda route handlers
- All 15 endpoints preserved and working:
  - GET /stats, GET /health (new), GET /guide, GET /chunk/:id
  - POST /chat, POST /clear-database, POST /sources, POST /tell-me-more
  - POST /reconstruct, POST /extract-ids
  - POST /query/code-discussions, POST /query/fixes, POST /query/feature
  - POST /add-file-path, POST /add-file
- CORS handling via set_pre_routing_handler (OPTIONS returns 204)
- Graceful shutdown: signal_handler calls svr.stop()
- Removed raw socket includes (sys/socket.h, netinet/in.h, arpa/inet.h)
- No more write() warning
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS (no warnings) | Backup: backups/ocean_chat_server_pre_step6.cpp

## Step 7: Add Request Logging (Feb 5, 2026)
- Added `svr.set_logger()` in run_http_server()
- Logs timestamp, method, path, status, body size for every request
- OPTIONS (CORS preflight) requests filtered out to reduce noise
- Format: `[2026-02-05 10:30:00] POST /chat -> 200 (1234B)`
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step7.cpp

## Step 12: TAAT Inverted-Index Search (Feb 5, 2026)
- **Replaced brute-force O(5M) search with TAAT inverted-index traversal**
- Old approach: iterate ALL 5M docs, build per-doc term_freq, compute BM25 (~900ms)
- New approach: for each query term, traverse only matching docs via posting lists
  - Uses g_stem_to_keywords to resolve stemmed terms to original keywords
  - Iterates posting lists from corpus.inverted_index (typically 10K-500K docs)
  - Document score accumulator (unordered_map) avoids duplicate processing
  - partial_sort for top-K extraction (no full sort needed)
- **Search times: 0-96ms (from 900-1400ms) = 10-100x faster!**
  - Q1 "Who is Tony Balay": 0ms (was 958ms)
  - Q5 "whale migration": 96ms (was 1045ms)
  - Q9 "September 22 BBQ": 31ms (was 1330ms)
- Removed OpenMP parallel brute-force loop (no longer needed)
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step12.cpp

## Step 11: Memory-Mapped Manifest Loading (Feb 5, 2026)
- Added `load_binary_manifest_mmap()` to binary_manifest.hpp
  - Uses `mmap()` + `MADV_SEQUENTIAL` for direct memory-mapped file access
  - Parses header, keyword dictionary, and chunk entries from mapped memory
  - Calls `madvise(MADV_DONTNEED)` + `munmap()` after parsing to release pages
- Added `#include <sys/mman.h>`, `<fcntl.h>`, `<unistd.h>` to binary_manifest.hpp
- Updated main server to use mmap loader instead of ifstream
- **Loading: 11.8s (mmap) vs 13.3s (ifstream) = ~12% faster**
- **RSS: 7.5 GB** (comparable to ifstream -- strings still need copying)
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step11.cpp, binary_manifest_pre_step11.hpp

## Step 10: Improve Keyword Extraction (Feb 5, 2026)
- Added `ABBREV_WHITELIST` (30 entries): AI, ML, US, UK, EU, DB, IP, ID, UI, etc.
  - 2-char abbreviations now kept as keywords (previously filtered out by >=3 rule)
- Added `STOP_WORDS` set (80+ entries): the, and, for, are, but, this, that, etc.
  - Common English words filtered out to reduce noise in search index
- Updated `extract_text_keywords()` to use both lists
- Added `#include <unordered_set>` for efficient lookup
- **Note:** Only affects queries and newly added files (existing index unchanged)
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step10.cpp

## Step 9: Add Porter Stemmer (Feb 5, 2026)
- Created `porter_stemmer.hpp` (241 lines) - classic Porter 1980 algorithm
  - C-style buffer with int indices (no size_t underflow)
  - All 5 steps: plurals, past tense, adjective suffixes, derivational, cleanup
- Added `g_stem_cache` (keyword -> stem) and `g_stem_to_keywords` (stem -> keywords)
- `build_stemmed_index()` at startup: stems all 1,141,068 unique keywords
  - Produces 1,071,046 unique stems in ~0.74 seconds
  - Lightweight: no posting list copies, just keyword mapping
- Updated `search_bm25()`:
  - Query terms stemmed before matching
  - Per-doc keywords stemmed via cache lookup
  - df computed from original inverted index via stem-to-keywords mapping
- Search times ~900-1400ms (higher than ~500-700ms without stemming due to
  per-doc cache lookups -- will be addressed by BlockWeakAnd in Step 12)
- **Accuracy: 10/10 (100%) PASSED**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step9.cpp

## Step 8: Add Auth and Rate Limiting (Feb 5, 2026)
- Added `auth` and `rate_limit` sections to Config struct and config.json
- Added `RateLimiter` class using token bucket algorithm (per-IP, gradual refill)
- Auth: optional X-API-Key header check (disabled by default)
  - Env var override: `OCEAN_SERVER_API_KEY`
  - Returns 401 with JSON error if key missing/wrong
- Rate limiting: optional per-IP request throttling (disabled by default)
  - Configurable `requests_per_minute` (default 60)
  - Returns 429 with JSON error when exceeded
- `/health` endpoint bypasses both auth and rate limiting
- Both integrated into `set_pre_routing_handler` (CORS still handled first)
- Startup messages show auth/rate-limit status when enabled
- **Tested: Auth 401 on wrong key, 200 on correct key, health bypasses**
- **Tested: Rate limit 429 after 5 req/min threshold exceeded**
- **Accuracy: 10/10 (100%) PASSED (with both disabled)**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step8.cpp

## Step 5: Add Graceful Shutdown (Feb 5, 2026)
- Added `signal.h` include and SIGINT/SIGTERM signal handlers
- Added `g_shutdown_requested` atomic flag and `g_server_fd` for socket close
- Signal handler closes server socket to unblock accept() loop
- `run_http_server()` loop checks `g_shutdown_requested`
- main() installs signal handlers before any other initialization
- **Tested: SIGINT cleanly stops the server**
- Compile: SUCCESS | Backup: backups/ocean_chat_server_pre_step5.cpp
