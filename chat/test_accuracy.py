#!/usr/bin/env python3
"""OceanEterna v4.2 Speed & Accuracy Test Suite
Tests search quality and performance across the 5M+ chunk Gutenberg corpus."""
import requests
import json
import time
import sys
import statistics

BASE = "http://localhost:9090"

# check server
try:
    r = requests.get(f"{BASE}/health", timeout=5)
    if r.json().get("status") != "ok":
        print("Server not ready"); sys.exit(1)
except:
    print("Cannot connect"); sys.exit(1)

stats = requests.get(f"{BASE}/stats").json()
print(f"Server: v{requests.get(f'{BASE}/health').json().get('version')}")
print(f"Corpus: {stats.get('chunks_loaded'):,} chunks, {stats.get('total_tokens'):,} tokens")
print(f"DB size: {stats.get('db_size_mb'):,} MB")

passed = 0
failed = 0
search_times = []
llm_times = []

def test(name, condition, detail=""):
    global passed, failed
    if condition:
        passed += 1
        print(f"  [PASS] {name}")
    else:
        failed += 1
        print(f"  [FAIL] {name}")
        if detail:
            print(f"         {detail[:300]}")

def section(title):
    print(f"\n{'='*70}")
    print(f"  {title}")
    print(f"{'='*70}")

# ============================================================
section("1. Search Speed Benchmarks (BM25 only, no LLM)")
# ============================================================

# queries designed to test different patterns on a Gutenberg corpus
speed_queries = [
    "the history of england",
    "romeo and juliet love",
    "darwin evolution natural selection",
    "sherlock holmes detective mystery",
    "pride and prejudice marriage",
    "war and peace napoleon",
    "frankenstein monster creation",
    "alice wonderland rabbit hole",
    "moby dick whale captain ahab",
    "great expectations pip",
    "tale of two cities revolution",
    "oliver twist orphan london",
    "dracula vampire transylvania",
    "adventures of tom sawyer",
    "the art of war strategy",
    "communist manifesto workers",
    "origin of species biology",
    "divine comedy dante inferno",
    "iliad troy achilles",
    "odyssey homer voyage",
]

for q in speed_queries:
    start = time.time()
    r = requests.post(f"{BASE}/chat", json={"question": q}, timeout=120)
    total_ms = (time.time() - start) * 1000
    data = r.json()
    search_ms = data.get("search_time_ms", 0)
    chunks_ret = data.get("chunks_retrieved", 0)
    search_times.append(search_ms)
    llm_times.append(total_ms - search_ms)

    test(f"'{q[:35]}' search={search_ms:.0f}ms chunks={chunks_ret}",
         search_ms < 500 and chunks_ret > 0,
         f"search_ms={search_ms}, chunks={chunks_ret}")

print(f"\n  Search stats: min={min(search_times):.0f}ms, max={max(search_times):.0f}ms, "
      f"avg={statistics.mean(search_times):.0f}ms, median={statistics.median(search_times):.0f}ms")

# ============================================================
section("2. Dynamic top_k Verification")
# ============================================================

r = requests.post(f"{BASE}/chat", json={"question": "machine learning artificial intelligence"}, timeout=120)
data = r.json()
chunks_ret = data.get("chunks_retrieved", 0)
# with 5M chunks, log2(5000000) ≈ 22
test(f"Dynamic top_k scales with corpus (got {chunks_ret})", chunks_ret >= 15,
     f"expected >=15 for 5M corpus, got {chunks_ret}")
test("top_k > old fixed value of 3", chunks_ret > 3)

# ============================================================
section("3. Search Relevance — Known Gutenberg Works")
# ============================================================

relevance_tests = [
    {
        "query": "romeo juliet love tragedy shakespeare",
        "expect_in_answer": ["romeo", "juliet"],
        "description": "Should find Romeo and Juliet content"
    },
    {
        "query": "sherlock holmes watson detective baker street",
        "expect_in_answer": ["holmes", "watson"],
        "description": "Should find Sherlock Holmes content"
    },
    {
        "query": "darwin natural selection evolution species",
        "expect_in_answer": ["species", "natural"],
        "description": "Should find Origin of Species content"
    },
    {
        "query": "pride prejudice elizabeth darcy bennett",
        "expect_in_answer": ["elizabeth", "darcy"],
        "description": "Should find Pride and Prejudice content"
    },
    {
        "query": "frankenstein monster creature laboratory",
        "expect_in_answer": ["creature", "monster"],
        "description": "Should find Frankenstein content"
    },
    {
        "query": "alice rabbit hole wonderland queen",
        "expect_in_answer": ["alice", "rabbit"],
        "description": "Should find Alice in Wonderland content"
    },
    {
        "query": "moby dick whale ahab pequod",
        "expect_in_answer": ["whale", "ahab"],
        "description": "Should find Moby Dick content"
    },
    {
        "query": "war peace napoleon moscow russia",
        "expect_in_answer": ["napoleon", "moscow"],
        "description": "Should find War and Peace content"
    },
]

for rt in relevance_tests:
    r = requests.post(f"{BASE}/chat", json={"question": rt["query"]}, timeout=120)
    data = r.json()
    sources = data.get("sources", [])

    # check relevance by fetching actual chunk content (LLM may be unavailable)
    all_content = ""
    # skip conversation turn chunks (CH*), check corpus chunks
    corpus_sources = [s for s in sources if not s.get("chunk_id", "").startswith("CH")]
    for src in corpus_sources[:5]:  # check top 5 corpus chunks
        cr = requests.get(f"{BASE}/chunk/{src['chunk_id']}", timeout=10)
        cd = cr.json()
        all_content += " " + cd.get("content", "").lower()

    found = [kw for kw in rt["expect_in_answer"] if kw in all_content]
    test(f"{rt['description']} ({len(found)}/{len(rt['expect_in_answer'])} keywords in chunks)",
         len(found) >= 1,
         f"checked {len(corpus_sources[:5])} chunks, content preview: {all_content[:200]}")

# ============================================================
section("4. Concurrent Search Performance")
# ============================================================

import threading
import concurrent.futures

concurrent_queries = [
    "history of mathematics",
    "greek mythology gods",
    "philosophy of mind",
    "economic theory capitalism",
    "french revolution liberty",
]

concurrent_times = []
concurrent_errors = []

def run_query(q):
    try:
        start = time.time()
        r = requests.post(f"{BASE}/chat", json={"question": q}, timeout=120)
        elapsed = (time.time() - start) * 1000
        data = r.json()
        return {"query": q, "search_ms": data.get("search_time_ms", 0),
                "total_ms": elapsed, "chunks": data.get("chunks_retrieved", 0),
                "success": True}
    except Exception as e:
        return {"query": q, "success": False, "error": str(e)}

with concurrent.futures.ThreadPoolExecutor(max_workers=5) as executor:
    futures = [executor.submit(run_query, q) for q in concurrent_queries]
    results = [f.result() for f in concurrent.futures.as_completed(futures)]

for res in results:
    if res["success"]:
        concurrent_times.append(res["search_ms"])
        test(f"Concurrent '{res['query'][:30]}' search={res['search_ms']:.0f}ms",
             res["search_ms"] < 1000 and res["chunks"] > 0)
    else:
        concurrent_errors.append(res)
        test(f"Concurrent '{res['query'][:30]}' failed", False, res.get("error", ""))

test("No concurrent errors", len(concurrent_errors) == 0,
     f"{len(concurrent_errors)} errors")
if concurrent_times:
    test("Concurrent avg search < 500ms",
         statistics.mean(concurrent_times) < 500,
         f"avg={statistics.mean(concurrent_times):.0f}ms")

# ============================================================
section("5. Edge Cases & Stress")
# ============================================================

# very short query
r = requests.post(f"{BASE}/chat", json={"question": "a"}, timeout=120)
data = r.json()
test("Single char query doesn't crash", r.status_code == 200)

# very long query
long_q = "the " * 500 + "end"
r = requests.post(f"{BASE}/chat", json={"question": long_q}, timeout=120)
data = r.json()
test("Very long query (2000+ chars) doesn't crash", r.status_code == 200)

# unicode query
r = requests.post(f"{BASE}/chat", json={"question": "über café résumé naïve"}, timeout=120)
data = r.json()
test("Unicode query doesn't crash", r.status_code == 200)

# empty-ish query
r = requests.post(f"{BASE}/chat", json={"question": "   "}, timeout=120)
data = r.json()
test("Whitespace query handled", r.status_code in [200, 400])

# special characters
r = requests.post(f"{BASE}/chat", json={"question": "C++ O'Brien don't \"quoted\""}, timeout=120)
data = r.json()
test("Special chars in query handled", r.status_code == 200)

# ============================================================
section("6. Catalog Browsing")
# ============================================================

r = requests.get(f"{BASE}/catalog?page_size=10")
cat = r.json()
total = cat.get("total_chunks", 0)
types = cat.get("types", {})
sources = cat.get("source_files", [])

test(f"Catalog total matches stats ({total:,})", abs(total - stats.get("chunks_loaded", 0)) < 100)
test(f"Catalog has type breakdown ({len(types)} types)", len(types) > 0)
test(f"Catalog has source files ({len(sources)} files)", len(sources) > 0)

# pagination test
r1 = requests.get(f"{BASE}/catalog?page_size=5&page=1")
r2 = requests.get(f"{BASE}/catalog?page_size=5&page=2")
c1 = r1.json().get("chunks", [])
c2 = r2.json().get("chunks", [])
if c1 and c2:
    ids1 = {c["chunk_id"] for c in c1}
    ids2 = {c["chunk_id"] for c in c2}
    test("Pagination returns different chunks", ids1 != ids2)
else:
    test("Pagination returns chunks", False, f"page1={len(c1)}, page2={len(c2)}")

# ============================================================
section("7. Adjacent Chunk Retrieval on Corpus")
# ============================================================

# pre-indexed corpus chunks won't have cross-refs (added in v4.2)
# test with a newly ingested chunk that has cross-refs
r = requests.get(f"{BASE}/catalog?source=ml_guide.txt&page_size=10")
ml_chunks = r.json().get("chunks", [])
if len(ml_chunks) >= 2:
    chunk_id = ml_chunks[1].get("chunk_id", "")
    r = requests.get(f"{BASE}/chunk/{chunk_id}?context_window=1")
    data = r.json()
    adj = data.get("adjacent_chunks", [])
    test(f"Adjacent retrieval on new chunk (ml_guide)", data.get("success") == True)
    test(f"Got adjacent chunks (context_window=1, got {len(adj)})", len(adj) > 0)
    if adj:
        test("Adjacent chunks have content", all(len(a.get("content", "")) > 0 for a in adj))
else:
    test("Need ml_guide chunks for adjacency test", False, f"only {len(ml_chunks)} chunks")

# also verify pre-indexed corpus chunks return 0 adjacent (expected — no cross-refs)
if c1:
    corpus_chunk = c1[0].get("chunk_id", "")
    r = requests.get(f"{BASE}/chunk/{corpus_chunk}?context_window=2")
    data = r.json()
    adj = data.get("adjacent_chunks", [])
    test(f"Pre-indexed corpus chunk fetch works", data.get("success") == True)
    test(f"Pre-indexed chunk: 0 adjacent (no cross-refs, expected)", len(adj) == 0)

# ============================================================
# Summary
# ============================================================
print(f"\n{'='*70}")
print(f"  RESULTS: {passed} passed, {failed} failed, {passed+failed} total")
print(f"{'='*70}")
print(f"\n  Search Performance Summary:")
print(f"    Sequential: min={min(search_times):.0f}ms, max={max(search_times):.0f}ms, "
      f"avg={statistics.mean(search_times):.0f}ms")
if concurrent_times:
    print(f"    Concurrent: min={min(concurrent_times):.0f}ms, max={max(concurrent_times):.0f}ms, "
          f"avg={statistics.mean(concurrent_times):.0f}ms")
print(f"    Corpus: {stats.get('chunks_loaded'):,} chunks")

if failed > 0:
    sys.exit(1)
