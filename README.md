# OceanEterna RAG System v4.1

**Date:** February 7, 2026
**Location:** `/home/yeblad/OE_1.24.26_v4/`

---

## Overview

OceanEterna is a high-performance RAG (Retrieval-Augmented Generation) system that uses BM25 keyword search instead of embeddings, enabling extremely fast indexing on CPU without GPU or API costs.

**v4.1** adds thread safety, SSE streaming, improved BM25 scoring with term frequency, and bug fixes. See [V4_CHANGELOG.md](V4_CHANGELOG.md) for details.

---

## Performance Summary

| Metric | v3 (Previous) | v4 (Current) | Improvement |
|--------|---------------|--------------|-------------|
| **Startup Time** | 13 seconds | 12 seconds | ~8% faster (mmap) |
| **Search Speed** | ~700ms | **0-100ms** | **10-100x faster** |
| **Accuracy** | 100% | 100% | Maintained |
| **Corpus** | 5M chunks | 5M chunks | Same |

---

## Quick Start

```bash
cd /home/yeblad/OE_1.24.26_v4/chat

# Set API key
export OCEAN_API_KEY="your-api-key"

# Start the server
./ocean_chat_server_v4

# Test a query
curl -X POST http://localhost:8888/chat \
  -H "Content-Type: application/json" \
  -d '{"question":"What is photosynthesis"}'
```

---

## What's New in v4

### Search Performance (10-100x faster)
- TAAT inverted-index traversal replaces brute-force O(5M) scan
- Porter stemming for morphological normalization
- Improved keyword extraction with stop words and abbreviation whitelist
- Typical queries: 0-100ms (was 700-1400ms)

### Production Readiness
- **cpp-httplib** HTTP server (replaces raw sockets)
- **Graceful shutdown** via SIGINT/SIGTERM
- **LLM retry** with exponential backoff (timeout, rate limit, 502/503)
- **Request logging** with timestamps
- **Optional auth** (X-API-Key header)
- **Optional rate limiting** (per-IP token bucket)

### Configuration
- `config.json` for all settings (no recompilation needed)
- Environment variable overrides (OCEAN_API_KEY, OCEAN_API_URL, etc.)
- No API keys in source code

### Architecture
- Modular header files (config, search, LLM, stemmer)
- Main server: 1,950 lines (was 2,382)
- See [ARCHITECTURE.md](ARCHITECTURE.md)

### Compression
- Zstd for new data (better ratio than LZ4)
- Auto-detection decompresses both LZ4 and Zstd transparently

---

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Health check (bypasses auth/rate-limit) |
| `/stats` | GET | System statistics |
| `/guide` | GET | Chapter navigation guide |
| `/chunk/:id` | GET | Retrieve chunk by ID |
| `/chat` | POST | Query with LLM answer |
| `/chat/stream` | POST | **v4.1** SSE streaming with progressive tokens |
| `/sources` | POST | Get source refs for a turn |
| `/tell-me-more` | POST | Expand on previous answer |
| `/reconstruct` | POST | Rebuild text from chunk IDs |
| `/extract-ids` | POST | Extract chunk IDs from text |
| `/query/code-discussions` | POST | Search code discussions |
| `/query/fixes` | POST | Search bug fixes |
| `/query/feature` | POST | Search feature requests |
| `/add-file` | POST | Upload file content to index |
| `/add-file-path` | POST | Index file from disk path |
| `/clear-database` | POST | Clear conversation history |

See [API.md](API.md) for full documentation.

---

## Building from Source

```bash
cd /home/yeblad/OE_1.24.26_v4/chat

g++ -O3 -std=c++17 -march=native -fopenmp \
    -o ocean_chat_server_v4 ocean_chat_server.cpp \
    -llz4 -lcurl -lpthread -lzstd
```

### Dependencies
- g++ with C++17 support
- liblz4-dev (LZ4 compression)
- libcurl4-openssl-dev (HTTP client)
- libzstd-dev (Zstd compression)

---

## Testing

```bash
# Accuracy test (10 questions, must pass 10/10)
export OCEAN_API_KEY="your-key"
python3 accuracy_test.py

# Comprehensive test (50 tests across 7 categories)
python3 test_comprehensive.py
```

---

## File Structure

```
OE_1.24.26_v4/
├── README.md               # This file
├── ARCHITECTURE.md          # Module diagram and data flow
├── API.md                   # API endpoint reference
├── CONFIGURATION.md         # config.json reference
├── V4_CHANGELOG.md          # Step-by-step v4 changes
└── chat/
    ├── ocean_chat_server_v4     # Server binary
    ├── ocean_chat_server.cpp    # Main server (~1950 lines)
    ├── config.hpp               # Config system
    ├── search_engine.hpp        # BM25 TAAT search
    ├── llm_client.hpp           # LLM API client
    ├── porter_stemmer.hpp       # Porter stemming
    ├── binary_manifest.hpp      # Binary manifest I/O
    ├── httplib.h                # cpp-httplib v0.18.3
    ├── json.hpp                 # nlohmann JSON
    ├── config.json              # Runtime configuration
    ├── ocean_demo.html          # Web UI
    ├── accuracy_test.py         # 10-question accuracy gate
    ├── test_comprehensive.py    # 47-test comprehensive suite
    ├── backups/                 # Pre-step backups
    └── guten_9m_build/
        ├── manifest_guten9m.bin     # Binary manifest (1.4 GB)
        ├── manifest_guten9m.jsonl   # JSONL manifest (3.3 GB)
        ├── chapter_guide.json       # Navigation guide
        └── storage/
            └── guten9m.bin          # Compressed chunks (7.4 GB)
```

---

## Version History

| Version | Date | Key Changes |
|---------|------|-------------|
| v1.0 | Jan 30, 2026 | Initial release, 100% accuracy |
| v2.0 | Feb 1, 2026 | Added BM25S engine |
| v3.0 | Feb 1, 2026 | Binary manifest, 3x faster startup |
| v4.0 | Feb 5, 2026 | 10-100x faster search, modular, production-ready |
| **v4.1** | Feb 7, 2026 | Thread safety, SSE streaming, TF-aware BM25, bug fixes |

---

## What's New in v4.1

### Thread Safety
- `shared_mutex` for reader-writer locking on corpus access
- `atomic<int>` for chat counter
- Safe concurrent request handling

### SSE Streaming (`/chat/stream`)
- Progressive token display via Server-Sent Events
- Search metadata sent immediately (before LLM)
- Frontend updated with streaming cursor animation
- Fallback to regular `/chat` if streaming fails

### Improved BM25 Scoring
- Term frequency (TF) tracking for new documents
- Full BM25 formula: `score = idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * dl/avgdl))`
- Legacy documents (5M chunks) maintain tf=1 for compatibility

### Bug Fixes
- LZ4 buffer safety (iterative decompression with growth)
- Eliminated double decompression in `/chat`
- Fixed test suite false positives
- Removed dead code

---

## License

Internal use only.
