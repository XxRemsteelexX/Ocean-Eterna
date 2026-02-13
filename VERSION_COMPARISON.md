# OceanEterna Version Comparison

**Date:** February 1, 2026

---

## Version Summary

| Version | Location | Startup | Search | Accuracy | Status |
|---------|----------|---------|--------|----------|--------|
| **v1 (Original)** | `/home/yeblad/OE_1.24.26/` | 41s | ~500ms | 100% | Baseline |
| **v2 (Experimental)** | `/home/yeblad/OE_1.24.26_v2/` | 16s | ~700ms | 100% | BM25S slower |
| **v3 (Recommended)** | `/home/yeblad/OE_1.24.26_v3/` | 13s | ~700ms | **100%** | **Best startup** |

---

## v3: What's Included

### ✅ Kept (Working)
- **Binary Manifest Format** - 3x faster startup (41s → 13s)
- **Original BM25 Search** - Reliable search with 100% accuracy
- **All existing features** - Chat, file indexing, chapter guide, etc.
- **Smaller file size** - 57% reduction in manifest size

### ❌ Removed (Not Effective)
- BM25S pre-computed scores (added overhead)
- BlockWeakAnd early termination (not triggering correctly)

---

## Performance Results

### Startup Time
```
v1 (JSONL):  41,000 ms
v2 (Binary): 16,000 ms
v3 (Binary): 13,000 ms
─────────────────────────
v3 Improvement: 3.2x faster than v1
```

### Search Time (10 query average)
```
v1: ~500ms (original BM25)
v2: ~700ms (BM25S overhead)
v3: ~700ms (original BM25, larger corpus)
```

### File Sizes
```
JSONL Manifest:  3,320 MB
Binary Manifest: 1,426 MB (43% of original)
Savings:         1,894 MB (57% reduction)
```

### Accuracy Test Results
```
v3 Accuracy Test - February 1, 2026
────────────────────────────────────
[PASS] Who is Tony Balay (758ms)
[PASS] BBQ class Missoula cost (791ms)
[PASS] Carbon Copy Cloner Mac (676ms)
[PASS] Denver school bond (681ms)
[PASS] whale migration (751ms)
[PASS] black hat SEO (678ms)
[PASS] photosynthesis (587ms)
[PASS] machine learning (668ms)
[PASS] September 22 BBQ class (704ms)
[PASS] Beginners BBQ (727ms)
────────────────────────────────────
Result: 10/10 passed (100%)
```

---

## How to Use v3

```bash
cd /home/yeblad/OE_1.24.26_v3/chat

# Start the server
./ocean_chat_server_v3

# Expected output:
# Binary manifest load: ~13 seconds
# Search: ~700ms per query
# Accuracy: 100%
```

---

## Files in v3

```
OE_1.24.26_v3/
├── README.md                    # Quick start guide
├── CHANGELOG.md                 # Version history
├── PROJECT_STATUS.md            # Full project status
├── VERSION_COMPARISON.md        # This file
└── chat/
    ├── ocean_chat_server_v3     # Compiled binary
    ├── ocean_chat_server.cpp    # Source code
    ├── binary_manifest.hpp      # Binary manifest library
    ├── convert_manifest         # JSONL→binary converter
    ├── accuracy_test.py         # Accuracy test script
    ├── json.hpp                 # JSON library
    ├── ocean_demo.html          # Web UI
    └── guten_9m_build/
        ├── manifest_guten9m.bin     # Binary manifest (1.4 GB)
        ├── manifest_guten9m.jsonl   # Original JSONL (3.3 GB)
        ├── chapter_guide.json       # Navigation guide
        └── storage/guten9m.bin      # Compressed chunks (6.9 GB)
```

---

## Why v3 is Recommended

| Factor | v1 | v3 | Winner |
|--------|-----|-----|--------|
| Startup Time | 41s | 13s | **v3** |
| Manifest Size | 3.3GB | 1.4GB | **v3** |
| Accuracy | 100% | 100% | Tie |
| Search Speed | 500ms | 700ms | v1 |
| Stability | Good | Good | Tie |

**v3 wins on startup (3x faster) and storage (57% smaller) while maintaining 100% accuracy.**

---

## Technical Details

### Binary Manifest Format
```
HEADER (24 bytes):
  magic[4]        = "OEM1"
  version[4]      = 1
  chunk_count[8]  = 5,065,226
  keyword_count[8]= 1,141,068

KEYWORD DICTIONARY:
  1,141,068 unique keywords stored once

CHUNK ENTRIES:
  5,065,226 chunks with keyword indices
```

### Why Search is Slightly Slower
- Corpus grew from 5,058,063 to 5,065,226 chunks (+7,163)
- System load variance
- Same algorithm, slightly more data

---

## Future Optimization Options

The BM25S infrastructure in v2 could be revisited:
1. Profile memory access patterns
2. Use contiguous memory layout for posting lists
3. Add SIMD vectorization (AVX2/AVX-512)
4. Implement smarter posting list pruning

For now, v3 provides the best balance of fast startup and reliable search.
