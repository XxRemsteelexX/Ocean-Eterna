# Ocean Eterna v7.11 / v8.0 / v9.0

**96% recall accuracy across 1 billion tokens. 12ms. 2.5GB RAM. No GPU.**

Ocean Eterna is a BM25 search engine that finds the right answer 96 out of 100 times across a billion tokens of text — tested on 200 ground-truth questions with mechanical scoring, no LLM judge. Pure C++, runs on a laptop.

## Scale Tiers

| Tier | Range | R@8 | RAM | Latency | Status |
|------|-------|-----|-----|---------|--------|
| **OE-Kraken** | Up to 2.5B tokens | 96% | 1-2.5 GB | <12ms | Production ready |
| **OE-Leviathan** | 2.5B - 15B tokens | 94-96% | 3-18 GB | <16ms | Production ready |
| **OE-Poseidon** | 15B - 30B tokens | ~92% | 10-20 GB | <20ms | Coming soon |
| **OE-Oceanus** | 30B - 50B tokens | TBD | TBD | TBD | In development |

## Versions in This Repo

### v7.11 (OE-Kraken)
The 1B champion. In-memory BM25 with TF-squared-IDF keyword scoring and cap-200 keywords per chunk. This configuration is baked into v8.0 as the default — there is no separate v7.11 binary.

### v8.0 (OE-Leviathan)
Streaming builder + modular shards for 5B-15B scale. Builds corpora in low-memory streaming passes, then serves via modular shard architecture.

- `ocean_chat_server.cpp` — HTTP chat server with BM25 search + LLM integration
- `bulk_build.cpp` — streaming corpus builder (low-memory, handles 5B+ tokens)
- `search_engine.hpp` — BM25 TAAT search with TF-squared-IDF, Porter stemming
- `binary_manifest.hpp` — serialized index format
- `mmap_index.hpp` — zero-copy memory-mapped index
- `porter_stemmer.hpp` — Porter stemming implementation

### v9.0 (OE-Poseidon / OE-Oceanus)
Two-pass disk-backed builder + mmap inverted index + multi-segment architecture. Adds hot-reloadable segments and per-segment BM25 for 15B+ scale.

Additional files:
- `config.hpp` — configuration constants
- `llm_client.hpp` — LLM reranking client
- `build_segments.sh` — segment build script
- `config.json` / `meta_example.json` — example configurations

## Dependencies

Header-only (included in repo root):
- `json.hpp` — [nlohmann/json](https://github.com/nlohmann/json)
- `httplib.h` — [cpp-httplib](https://github.com/yhirose/cpp-httplib)

System libraries:
```
sudo apt install liblz4-dev libcurl4-openssl-dev libzstd-dev
```

## Build

```bash
# v8.0 server
g++ -O3 -std=c++17 -march=native -fopenmp \
  v8.0/ocean_chat_server.cpp \
  -I. -o ocean_chat_server \
  -llz4 -lcurl -lzstd -lpthread

# v8.0 bulk builder (for 5B+ corpora)
g++ -O3 -std=c++17 -march=native -fopenmp \
  v8.0/bulk_build.cpp \
  -I. -o bulk_build \
  -llz4 -lzstd -lpthread
```

## Run

```bash
./ocean_chat_server 8888
```

Then:
```bash
curl -X POST http://localhost:8888/chat \
  -H "Content-Type: application/json" \
  -d '{"question": "How does authentication work?"}'
```

## Benchmark Results

Tested on i9-14900KS / 96GB DDR5 / NVMe SSD:

| Scale | Tier | R@8 | R@1 | Hard R@8 | RAM | p50 Latency |
|-------|------|-----|-----|----------|-----|-------------|
| 1B tokens | OE-Kraken | 96.0% | 53.0% | 92.0% | 2,484 MB | 11.94ms |
| 5B tokens | OE-Leviathan | 94.5% | 40.5% | 86.0% | 12,221 MB | 13.4ms |
| 10B tokens | OE-Leviathan | 94.0% | 52.0% | 82.0% | 17,738 MB | 15.5ms |

Indexing speed: 70M tokens/sec.

## API

14 endpoints including:
- `POST /chat` — BM25 search + LLM response
- `POST /chat/stream` — streaming search (SSE)
- `POST /add-file` — ingest text content
- `POST /add-file-path` — ingest from filesystem
- `GET /stats` — corpus statistics
- `GET /health` — server health

## License

BSL 1.1 — Free for personal and internal business use. Converts to Apache 2.0 on 2030-03-08.

Commercial licensing: [contact chAIn](https://chainlinks.ai/contact)

## Links

- Website: [oceaneterna.com](https://oceaneterna.com)
- Company: [chainlinks.ai](https://chainlinks.ai)
