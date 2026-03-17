# Ocean Eterna

**91% Recall@8 at 50 billion tokens. 37-second cold start. Sub-100ms search. No GPU.**

Ocean Eterna is a BM25 search engine built in C++. 99.5% recall at 500M tokens, 97% at 1B, 91% at 50B. Tested on 200 ground-truth questions with mechanical scoring, no LLM judge. Single binary, runs on a Raspberry Pi.

## Product Tiers

| Tier | Engine | Range | R@8 | RAM | Search p50 | Architecture |
|------|--------|-------|-----|-----|-----------|-------------|
| **OE Kraken** | v9.0 | 20M-4B tokens | 96-99.5% | 38 MB - 2.1 GB | 1.6-41ms | Single mmap inverted index |
| **OE Leviathan** | v9.3 | 5B-50B+ tokens | 90-95% | 3-12 GB | 20-92ms | Multi-segment (2.5B per segment) |

**OE Kraken** is the flagship. v9.0 mmap inverted index achieves the highest recall at any scale up to 4B tokens in minimal RAM. Runs on anything from a Raspberry Pi to a workstation.

**OE Leviathan** is the enterprise tier. v9.3 multi-segment architecture splits corpus into independent 2.5B-token segments, searches all in parallel (OpenMP), merges results. 91% R@8 at 50B tokens (9.5M chunks) with 37-second cold start.

## Benchmark Results

### OE Kraken (v9.0, single segment)

| Scale | R@8 | Hard R@8 | RAM | p50 | Build (1x) |
|-------|-----|----------|-----|-----|-----------|
| 20M | 99.0% | 96.0% | 38 MB | 1.6ms | 1.8s |
| 50M | 98.0% | 92.0% | 61 MB | 3.0ms | 4.5s |
| 250M | 99.0% | 96.0% | 557 MB | 8.9ms | 21.6s |
| 500M | **99.5%** | 100% | 259 MB | 12.5ms | 51.6s |
| 1B | **97.0%** | 88.0% | 319 MB | 10.7ms | ~100s |
| 2B | 99.0% | 96.0% | 1.5 GB | 30.1ms | ~6min |
| 3B | 98.0% | 92.0% | 2.1 GB | 40.9ms | ~10min |
| 4B | 96.0% | 84.0% | 2.1 GB | 12.6ms | ~13min |

200 corpus-verified ground-truth questions per scale. i9-14900KS / 96GB DDR5 / NVMe.

### OE Leviathan (v9.3, multi-segment)

| Scale | Segs | Chunks | R@8 | Hard R@8 | p50 | Build (1x) | Server Load |
|-------|------|--------|-----|----------|-----|-----------|-------------|
| 5B | 2 | 978K | 93.5% | 80.0% | 22ms | 8 min | 7s |
| 10B | 4 | 1.51M | 95.0% | 82.0% | 20ms | 11 min | 7s |
| 15B | 6 | 1.74M | 92.0% | 74.0% | 83ms | 24 min | 12s |
| 20B | 8 | 2.40M | 90.0% | 66.0% | 37ms | 33 min | 12s |
| 30B | 12 | 4.79M | 93.0% | 76.0% | 49ms | 69 min | 22s |
| **50B** | **20** | **9.55M** | **91.0%** | 66.0% | **92ms** | **193 min** | **37s** |

- **Build time** = one-time offline index creation (done once, segments stored on disk)
- **Server load** = cold start to serving queries (mmap all segments + load docs)
- Easy/medium recall never drops below 97% at any scale

## Quick Start

```bash
# build
g++ -O3 -std=c++17 -march=native -fopenmp \
  v9.0/ocean_chat_server.cpp \
  -I. -o ocean_chat_server \
  -llz4 -lcurl -lzstd -lpthread

# run
export OCEAN_API_KEY=your_openrouter_key  # optional, for LLM features
./ocean_chat_server 8888

# search
curl -X POST http://localhost:8888/chat \
  -H "Content-Type: application/json" \
  -d '{"question": "How does authentication work?"}'
```

## Config

```json
{
  "server": {"port": 8888},
  "corpus": {
    "manifest": "corpus/manifest.jsonl",
    "storage": "corpus/storage.bin",
    "chapter_guide": "corpus/chapter_guide.json"
  },
  "llm": {"use_external": true},
  "reranker": {"enabled": false},
  "scanner": {"enabled": false}
}
```

Multi-segment mode activates automatically when `meta.json` exists in the corpus directory. No config flag needed.

## Source Code

### v9.0 (OE Kraken + OE Leviathan)

- `ocean_chat_server.cpp` -- HTTP server with BM25 search, multi-segment support, LLM integration
- `bulk_build.cpp` -- streaming corpus builder (low-memory, handles 50B+)
- `search_engine.hpp` -- BM25 search with TF-squared-IDF, multi-segment parallel search, Phase-2 coverage rescoring
- `mmap_index.hpp` -- zero-copy memory-mapped inverted index
- `binary_manifest.hpp` -- binary manifest format for fast loading
- `config.hpp` -- configuration system with env var overrides
- `porter_stemmer.hpp` -- Porter stemming
- `build_segments.sh` -- multi-segment build script

### v8.0 (legacy, in-memory)

In-memory BM25 engine. 94-96% R@8 from 1B to 20B, but requires 3-60GB RAM. Superseded by v9.0/v9.3 multi-segment for most use cases.

## Build

See [BUILD.md](BUILD.md) for full instructions.

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Dependencies

Header-only (included): `json.hpp` (nlohmann/json), `httplib.h` (cpp-httplib)

System:
```
sudo apt install build-essential cmake libzstd-dev liblz4-dev libcurl4-openssl-dev libomp-dev
```

## API

14 endpoints including:
- `POST /chat` -- BM25 search + LLM response
- `POST /chat/stream` -- streaming search (SSE)
- `POST /search` -- raw BM25 search (no LLM)
- `POST /add-file` -- ingest text content
- `GET /stats` -- corpus statistics
- `GET /health` -- server health

## License

BSL 1.1 -- Free for personal and internal business use. Converts to Apache 2.0 on 2030-03-08.

Commercial licensing: [contact chAIn](https://chainlinks.ai/contact)

## Links

- Website: [oceaneterna.com](https://oceaneterna.com)
- Company: [chainlinks.ai](https://chainlinks.ai)
