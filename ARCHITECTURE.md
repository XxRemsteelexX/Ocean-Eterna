# OceanEterna v4 Architecture

## Module Diagram

```
ocean_chat_server.cpp (main, ~1950 lines)
    |
    +-- config.hpp          Config struct, load_config(), env vars
    +-- search_engine.hpp   BM25 TAAT search, keyword extraction, stemming
    +-- llm_client.hpp      CURL-based LLM API calls with retry
    +-- porter_stemmer.hpp  Porter 1980 stemming algorithm
    +-- binary_manifest.hpp Binary manifest I/O + mmap loader
    +-- httplib.h           cpp-httplib v0.18.3 (HTTP server)
    +-- json.hpp            nlohmann JSON (parsing)
```

## Data Flow

```
User Query
  |
  v
HTTP Server (httplib)
  |
  v
extract_text_keywords()  -->  Porter Stemmer
  |
  v
search_bm25() TAAT       -->  Inverted Index (5M+ docs)
  |
  v
decompress_chunk()        -->  LZ4 or Zstd (auto-detect)
  |
  v
query_llm()               -->  External API (with retry)
  |
  v
JSON Response --> Client
```

## Key Components

### ocean_chat_server.cpp
- `main()`: Config loading, corpus loading, server startup
- HTTP route handlers for all 15 endpoints
- Conversation management (turns, history, cache)
- File upload and chunking (parallel with OpenMP)
- Graceful shutdown via SIGINT/SIGTERM

### config.hpp (~120 lines)
- `Config` struct: server, llm, search, corpus, auth, rate_limit sections
- `load_config()`: JSON file loading with env var overrides
- Priority: env vars > config.json > compiled defaults

### search_engine.hpp (~172 lines)
- `extract_text_keywords()`: Tokenization with stop words and abbreviation whitelist
- `build_stemmed_index()`: Builds stem cache at startup (~0.7s for 1.1M keywords)
- `search_bm25()`: TAAT inverted-index traversal (0-100ms per query)
- BM25 scoring with configurable k1/b parameters

### llm_client.hpp (~155 lines)
- `query_llm_once()`: Single CURL call to LLM API
- `query_llm()`: Retry wrapper with exponential backoff (1s, 2s, 4s)
- Supports both local (LM Studio) and external (routeLLM) APIs
- Retries on: timeout, rate limit, 502, 503, connection errors

### porter_stemmer.hpp (~242 lines)
- Classic Porter 1980 algorithm (5 steps)
- C-style buffer with int indices (no size_t underflow)
- `porter::stem(word)` entry point

### binary_manifest.hpp (~685 lines)
- Binary format: OEM1 header + keyword dictionary + chunk entries
- `load_binary_manifest_mmap()`: Memory-mapped loading (~11.8s)
- `save_binary_manifest()`: Binary serialization

## Memory Layout

```
RAM (~7.5 GB):
  DocMeta array:     ~5M entries (id, summary, keywords, offset, length)
  Inverted index:    ~1.1M keywords -> posting lists
  Stem cache:        ~1.1M keyword -> stem mappings
  Stem-to-keywords:  ~1.07M stem -> keyword list mappings
  Conversation cache: In-memory turn history
```

## Compression

- Existing data: LZ4 frame format (auto-detected by magic bytes `04 22 4D 18`)
- New data: Zstd format (magic bytes `28 B5 2F FD`)
- Auto-detection in `decompress_chunk()` handles both transparently
- Storage file: ~7.4 GB binary blob (offset + length indexing)
