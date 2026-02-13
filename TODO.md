# OceanEterna - TODO Checklist

## Priority 1: Fix HTTP Bug

- [ ] Open `ocean_chat_server.cpp`
- [ ] Go to line ~1744 (HTTP request reading)
- [ ] Replace current read logic with:
  ```cpp
  // Read headers first, then read exact Content-Length bytes
  // Add select() or poll() with timeout to prevent hanging
  ```
- [ ] Rebuild: `g++ -O3 -std=c++17 -march=native -fopenmp -o ocean_chat_server ocean_chat_server.cpp -llz4 -lcurl -lpthread`
- [ ] Test: `curl -X POST http://localhost:8888/chat -H "Content-Type: application/json" -d '{"question":"Who is Tony Balay"}'`

## Priority 2: Test Accuracy

- [ ] Run test script: `python3 /tmp/test_ocean.py`
- [ ] Or test via browser: `file:///home/yeblad/OE_1.24.26/chat/ocean_demo.html`
- [ ] Expected: 80-100% accuracy on factual questions from indexed content

## Priority 3: Verify Search Performance

- [ ] Check search latency at 5M chunks
- [ ] Target: <500ms search time
- [ ] Current: ~550ms (observed in logs)

## Done

- [x] Parallel indexing (32 cores)
- [x] 5.9GB file in 53 seconds
- [x] UTF-8 sanitization
- [x] Paragraph boundary chunking
- [x] 5M+ chunks loaded
- [x] Server starts successfully

## Quick Start

```bash
cd /home/yeblad/OE_1.24.26/chat
./ocean_chat_server 8888
# Wait 40 seconds for 5M chunks to load
# Test: curl http://localhost:8888/health
```

## Files to Edit

| Task | File | Line |
|------|------|------|
| HTTP fix | ocean_chat_server.cpp | ~1744 |
| Test script | /tmp/test_ocean.py | - |

## Test Questions (from indexed data)

1. Who is Tony Balay? → BBQ Champion, Lonestar Smoke Rangers
2. BBQ class cost? → $35
3. Carbon Copy Cloner? → Mac drive cloning software
4. Denver school bond? → $572 million
5. Bangalore to Gondia train? → WAINGANGA EXP, 12251
6. Black Hat SEO? → Deceptive search techniques
7. Mac OS X Lion version? → 10.7
8. Niodor village? → Senegal fishing village
9. KCBS? → BBQ competition organization
10. Hip hop costume material? → Lycra, spandex, metallic
