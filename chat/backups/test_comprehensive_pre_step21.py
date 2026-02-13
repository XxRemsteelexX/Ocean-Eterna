#!/usr/bin/env python3
"""
OceanEterna v4 Comprehensive Test Suite
Tests all endpoints, edge cases, performance, and accuracy.
"""
import subprocess
import time
import json
import requests
import sys
import os
from concurrent.futures import ThreadPoolExecutor, as_completed

BASE = "http://localhost:8888"

# Kill any existing server
os.system("pkill -9 -f ocean_chat_server 2>/dev/null")
time.sleep(2)
os.system("touch guten_9m_build/manifest_guten9m.bin")

print("=" * 60)
print("  OceanEterna v4 Comprehensive Test Suite")
print("=" * 60)
print()

# Start server
print("Starting server...")
server = subprocess.Popen(
    ["./ocean_chat_server_v4"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT
)

print("Waiting for server to load...")
time.sleep(25)

# Verify server is up
try:
    r = requests.get(f"{BASE}/health", timeout=10)
    if r.status_code != 200:
        print("FATAL: Server failed to start")
        server.terminate()
        sys.exit(1)
except:
    print("FATAL: Cannot connect to server")
    server.terminate()
    sys.exit(1)

print("Server is ready!\n")

passed = 0
failed = 0
total = 0

def test(name, condition, detail=""):
    global passed, failed, total
    total += 1
    if condition:
        passed += 1
        print(f"  [PASS] {name}")
    else:
        failed += 1
        print(f"  [FAIL] {name}")
        if detail:
            print(f"         {detail[:120]}")

# ============================================================
# Section 1: Accuracy Tests (10 questions)
# ============================================================
print("-" * 60)
print("Section 1: Accuracy Tests (10 questions)")
print("-" * 60)

accuracy_tests = [
    ("Who is Tony Balay", ["tony", "balay"]),
    ("How much does the BBQ class in Missoula cost", ["$", "cost", "class"]),
    ("What is Carbon Copy Cloner for Mac", ["backup", "mac", "clone"]),
    ("What is the Denver school bond", ["denver", "school", "bond"]),
    ("Tell me about whale migration", ["whale", "migrat"]),
    ("What is black hat SEO", ["seo", "black hat", "search"]),
    ("What is photosynthesis", ["light", "plant", "energy"]),
    ("What is machine learning", ["learn", "algorithm", "data"]),
    ("When is the September 22 BBQ class", ["september", "22", "bbq"]),
    ("What is the Beginners BBQ class about", ["bbq", "beginner", "class"]),
]

last_turn_id = None
for i, (question, keywords) in enumerate(accuracy_tests, 1):
    try:
        r = requests.post(f"{BASE}/chat", json={"question": question}, timeout=60)
        data = r.json()
        answer = data.get("answer", "").lower()
        search_ms = data.get("search_time_ms", 0)
        if data.get("turn_id"):
            last_turn_id = data["turn_id"]
        found = any(kw.lower() in answer for kw in keywords)
        test(f"Q{i}: {question} ({search_ms:.0f}ms)", found,
             f"Answer: {answer[:100]}")
    except Exception as e:
        test(f"Q{i}: {question}", False, str(e))
    time.sleep(1)

# ============================================================
# Section 2: GET Endpoints
# ============================================================
print()
print("-" * 60)
print("Section 2: GET Endpoints")
print("-" * 60)

# /health
r = requests.get(f"{BASE}/health", timeout=10)
test("/health returns 200", r.status_code == 200)
data = r.json()
test("/health has status=ok", data.get("status") == "ok")
test("/health has version", "version" in data)

# /stats (response keys: chunks_loaded, total_tokens, total_queries, etc.)
r = requests.get(f"{BASE}/stats", timeout=10)
test("/stats returns 200", r.status_code == 200)
data = r.json()
test("/stats has chunks_loaded", "chunks_loaded" in data)
test("/stats chunks_loaded > 5M", data.get("chunks_loaded", 0) > 5000000)

# /guide (response: dict with version, chunks, etc.)
r = requests.get(f"{BASE}/guide", timeout=10)
test("/guide returns 200", r.status_code == 200)
data = r.json()
test("/guide has version", "version" in data)

# /chunk/:id - valid chunk (uses dot notation: guten9m.1)
r = requests.get(f"{BASE}/chunk/guten9m.1", timeout=10)
test("/chunk/guten9m.1 returns 200", r.status_code == 200)
data = r.json()
test("/chunk has content", data.get("success") == True and len(data.get("content", "")) > 0)

# /chunk/:id - invalid chunk
r = requests.get(f"{BASE}/chunk/nonexistent_999999", timeout=10)
test("/chunk/nonexistent handled", r.status_code == 200)
data = r.json()
test("/chunk/nonexistent success=false", data.get("success") == False)

# ============================================================
# Section 3: POST Endpoints
# ============================================================
print()
print("-" * 60)
print("Section 3: POST Endpoints")
print("-" * 60)

# /sources (requires turn_id from a previous /chat call)
if last_turn_id:
    r = requests.post(f"{BASE}/sources", json={"turn_id": last_turn_id}, timeout=30)
    test("/sources returns 200", r.status_code == 200)
    data = r.json()
    test("/sources has sources list", "sources" in data)
else:
    test("/sources (skipped - no turn_id)", False, "No turn_id from chat")
    test("/sources has sources list (skipped)", False)

# /tell-me-more (requires prev_turn_id)
if last_turn_id:
    r = requests.post(f"{BASE}/tell-me-more", json={"prev_turn_id": last_turn_id}, timeout=60)
    test("/tell-me-more returns 200", r.status_code == 200)
else:
    test("/tell-me-more (skipped - no turn_id)", False)

# /reconstruct (uses dot notation chunk IDs)
r = requests.post(f"{BASE}/reconstruct", json={"chunk_ids": ["guten9m.1", "guten9m.2"]}, timeout=30)
test("/reconstruct returns 200", r.status_code == 200)
data = r.json()
test("/reconstruct has context", data.get("success") == True and len(data.get("context", "")) > 0)

# /extract-ids
r = requests.post(f"{BASE}/extract-ids",
    json={"text": "See chunk guten9m.1 and guten9m.2"}, timeout=10)
test("/extract-ids returns 200", r.status_code == 200)
data = r.json()
test("/extract-ids finds IDs", "chunk_ids" in data and len(data.get("chunk_ids", [])) > 0)

# /query/code-discussions
r = requests.post(f"{BASE}/query/code-discussions",
    json={"question": "python debugging"}, timeout=30)
test("/query/code-discussions returns 200", r.status_code == 200)

# /query/fixes
r = requests.post(f"{BASE}/query/fixes",
    json={"question": "memory leak"}, timeout=30)
test("/query/fixes returns 200", r.status_code == 200)

# /query/feature
r = requests.post(f"{BASE}/query/feature",
    json={"question": "search optimization"}, timeout=30)
test("/query/feature returns 200", r.status_code == 200)

# /add-file (uses zstd compression now)
r = requests.post(f"{BASE}/add-file",
    json={"filename": "test_suite_file.txt",
          "content": "Comprehensive test of file upload. The OceanEterna system supports adding new documents to its search index in real time."},
    timeout=30)
test("/add-file returns 200", r.status_code == 200)
data = r.json()
test("/add-file success=true", data.get("success") == True)
test("/add-file chunks_added > 0", data.get("chunks_added", 0) > 0)

# /clear-database (can be slow - increase timeout)
r = requests.post(f"{BASE}/clear-database", timeout=60)
test("/clear-database returns 200", r.status_code == 200)

# ============================================================
# Section 4: Edge Cases
# ============================================================
print()
print("-" * 60)
print("Section 4: Edge Cases")
print("-" * 60)

# Empty question
r = requests.post(f"{BASE}/chat", json={"question": ""}, timeout=30)
test("Empty question doesn't crash", r.status_code in [200, 400])

# Very long question
long_q = "What is " + "really " * 200 + "important?"
r = requests.post(f"{BASE}/chat", json={"question": long_q}, timeout=60)
test("Long question (1400+ chars) handled", r.status_code == 200)

# Special characters
r = requests.post(f"{BASE}/chat",
    json={"question": "What about <script>alert('xss')</script> & 'quotes' \"double\"?"},
    timeout=30)
test("Special chars handled", r.status_code == 200)

# Unicode
r = requests.post(f"{BASE}/chat",
    json={"question": "Qu'est-ce que la photosynthese?"},
    timeout=30)
test("Unicode question handled", r.status_code == 200)

# Malformed JSON
r = requests.post(f"{BASE}/chat", data="not json",
    headers={"Content-Type": "application/json"}, timeout=10)
test("Malformed JSON handled", r.status_code in [200, 400])

# Missing field
r = requests.post(f"{BASE}/chat", json={"wrong_field": "test"}, timeout=30)
test("Missing 'question' field handled", r.status_code in [200, 400])

# ============================================================
# Section 5: Performance Tests
# ============================================================
print()
print("-" * 60)
print("Section 5: Performance Tests")
print("-" * 60)

perf_queries = [
    "Tony Balay",
    "whale migration patterns",
    "September 22 BBQ",
    "machine learning algorithms",
    "black hat SEO techniques",
]

search_times = []
for q in perf_queries:
    r = requests.post(f"{BASE}/chat", json={"question": q}, timeout=60)
    data = r.json()
    ms = data.get("search_time_ms", 9999)
    search_times.append(ms)
    time.sleep(1)

avg_ms = sum(search_times) / len(search_times)
max_ms = max(search_times)
test(f"Avg search time < 200ms (actual: {avg_ms:.0f}ms)", avg_ms < 200)
test(f"Max search time < 500ms (actual: {max_ms:.0f}ms)", max_ms < 500)

# ============================================================
# Section 6: Concurrent Requests
# ============================================================
print()
print("-" * 60)
print("Section 6: Concurrent Requests (10 threads)")
print("-" * 60)

concurrent_queries = [
    "Who is Tony Balay",
    "whale migration",
    "photosynthesis",
    "machine learning",
    "BBQ class cost",
    "Carbon Copy Cloner",
    "Denver school bond",
    "black hat SEO",
    "September BBQ",
    "beginner BBQ class",
]

results = []
errors = []

def send_query(q):
    try:
        r = requests.post(f"{BASE}/chat", json={"question": q}, timeout=120)
        return (q, r.status_code, r.json().get("answer", "")[:50])
    except Exception as e:
        return (q, 0, str(e))

with ThreadPoolExecutor(max_workers=10) as executor:
    futures = {executor.submit(send_query, q): q for q in concurrent_queries}
    for future in as_completed(futures):
        q, status, preview = future.result()
        results.append((q, status))
        if status != 200:
            errors.append(f"{q}: status={status}")

success_count = sum(1 for _, s in results if s == 200)
test(f"All 10 concurrent requests succeeded ({success_count}/10)", success_count == 10,
     "; ".join(errors) if errors else "")

# ============================================================
# Section 7: CORS
# ============================================================
print()
print("-" * 60)
print("Section 7: CORS")
print("-" * 60)

r = requests.options(f"{BASE}/chat",
    headers={"Origin": "http://example.com", "Access-Control-Request-Method": "POST"},
    timeout=10)
test("OPTIONS /chat returns 2xx", r.status_code in [200, 204])
cors_header = r.headers.get("Access-Control-Allow-Origin", "")
test("CORS Allow-Origin present", cors_header != "",
     f"Got: '{cors_header}'")

# ============================================================
# Summary
# ============================================================
print()
print("=" * 60)
print(f"  RESULTS: {passed}/{total} passed ({100*passed//total}%)")
if failed > 0:
    print(f"  {failed} tests FAILED")
else:
    print("  ALL TESTS PASSED!")
print("=" * 60)

# Cleanup
server.terminate()

# Clean up test file from manifest
os.system("python3 -c \"lines=open('guten_9m_build/manifest_guten9m.jsonl').readlines(); f=open('guten_9m_build/manifest_guten9m.jsonl','w'); f.writelines([l for l in lines if 'test_suite_file' not in l]); f.close()\"")
os.system("touch guten_9m_build/manifest_guten9m.bin")

sys.exit(0 if failed == 0 else 1)
