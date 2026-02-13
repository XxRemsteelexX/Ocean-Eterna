# Step-by-Step Plan: Fix HTTP Request Bug

## The Problem
POST requests to `/chat` endpoint return "Invalid request" because the server's `read()` call doesn't reliably receive the full HTTP body.

## File to Edit
`/home/yeblad/OE_1.24.26/chat/ocean_chat_server.cpp` - Line ~1744

---

## PHASE 1: Diagnose (5 steps)

### Step 1.1: Check current server status
```bash
curl http://127.0.0.1:8888/health
# If not running:
cd /home/yeblad/OE_1.24.26/chat
./ocean_chat_server 8888 &
sleep 50  # Wait for 5M chunks to load
```

### Step 1.2: Test current behavior
```bash
curl -v -X POST http://127.0.0.1:8888/chat \
  -H "Content-Type: application/json" \
  -d '{"question":"test"}'
```
Expected: "Invalid request" (confirms bug)

### Step 1.3: Check server logs
```bash
tail -50 /tmp/server_out.txt
```
Look for: What does server receive? Is body truncated?

### Step 1.4: Add debug logging to see what's received
In ocean_chat_server.cpp, after reading request, add:
```cpp
cerr << "[HTTP DEBUG] Received " << request.length() << " bytes" << endl;
cerr << "[HTTP DEBUG] Request: " << request.substr(0, 500) << endl;
```

### Step 1.5: Rebuild and test with debug
```bash
pkill -f ocean_chat_server
g++ -O3 -std=c++17 -march=native -fopenmp -o ocean_chat_server ocean_chat_server.cpp -llz4 -lcurl -lpthread
./ocean_chat_server 8888 > /tmp/server_out.txt 2>&1 &
sleep 50
curl -X POST http://127.0.0.1:8888/chat -H "Content-Type: application/json" -d '{"question":"test"}'
tail -20 /tmp/server_out.txt
```

---

## PHASE 2: Fix the Bug (4 steps)

### Step 2.1: Read the current HTTP handling code
```bash
# Read lines 1740-1800 of ocean_chat_server.cpp
```

### Step 2.2: Replace HTTP reading code at line ~1744

**CURRENT CODE (broken):**
```cpp
char buffer[65536] = {0};
string request;
int n = read(client_socket, buffer, sizeof(buffer) - 1);
if (n > 0) {
    request.append(buffer, n);
    // ... rest of code
}
```

**REPLACEMENT CODE (fixed):**
```cpp
// Set socket timeout to prevent hanging
struct timeval tv;
tv.tv_sec = 5;  // 5 second timeout
tv.tv_usec = 0;
setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

// Read request with proper Content-Length handling
char buffer[65536] = {0};
string request;
int total_read = 0;

// First read - get headers
int n = read(client_socket, buffer, sizeof(buffer) - 1);
if (n <= 0) {
    close(client_socket);
    continue;
}
request.append(buffer, n);
total_read = n;

// Find headers end
size_t headers_end = request.find("\r\n\r\n");
if (headers_end == string::npos) {
    // Keep reading until we find headers
    while (headers_end == string::npos && total_read < 100000) {
        memset(buffer, 0, sizeof(buffer));
        n = read(client_socket, buffer, sizeof(buffer) - 1);
        if (n <= 0) break;
        request.append(buffer, n);
        total_read += n;
        headers_end = request.find("\r\n\r\n");
    }
}

if (headers_end != string::npos) {
    // Parse Content-Length
    int content_length = 0;
    size_t cl_pos = request.find("Content-Length: ");
    if (cl_pos == string::npos) cl_pos = request.find("content-length: ");
    if (cl_pos != string::npos && cl_pos < headers_end) {
        content_length = atoi(request.c_str() + cl_pos + 16);
    }

    // Calculate how much body we have
    int body_start = headers_end + 4;
    int body_received = request.length() - body_start;

    // Read remaining body if needed
    while (body_received < content_length && total_read < 1000000) {
        memset(buffer, 0, sizeof(buffer));
        n = read(client_socket, buffer, sizeof(buffer) - 1);
        if (n <= 0) break;
        request.append(buffer, n);
        total_read += n;
        body_received = request.length() - body_start;
    }
}

// Debug log
cerr << "[HTTP] Read " << total_read << " bytes, request length: " << request.length() << endl;
```

### Step 2.3: Add required include at top of file (if not present)
```cpp
#include <sys/socket.h>  // for setsockopt, SO_RCVTIMEO
```

### Step 2.4: Rebuild
```bash
pkill -f ocean_chat_server
g++ -O3 -std=c++17 -march=native -fopenmp -o ocean_chat_server ocean_chat_server.cpp -llz4 -lcurl -lpthread 2>&1
# Check for errors - if none, continue
```

---

## PHASE 3: Test the Fix (5 steps)

### Step 3.1: Start server fresh
```bash
./ocean_chat_server 8888 > /tmp/server_out.txt 2>&1 &
sleep 50  # Wait for chunks to load
curl http://127.0.0.1:8888/health  # Should return OK
```

### Step 3.2: Test single question
```bash
curl -s -X POST http://127.0.0.1:8888/chat \
  -H "Content-Type: application/json" \
  -d '{"question":"Who is Tony Balay"}'
```
Expected: JSON response with answer about BBQ champion

### Step 3.3: Check server logs
```bash
tail -30 /tmp/server_out.txt
```
Should show: `[HTTP] Read XX bytes` with full request

### Step 3.4: Run test script
```bash
python3 /tmp/test_ocean.py
```
Expected: Most questions pass (80%+)

### Step 3.5: Test via browser demo
Open: file:///home/yeblad/OE_1.24.26/chat/ocean_demo.html
Type: "Who is Tony Balay"
Expected: Answer about BBQ champion

---

## PHASE 4: Verify Performance (3 steps)

### Step 4.1: Test search speed
```bash
time curl -s -X POST http://127.0.0.1:8888/chat \
  -H "Content-Type: application/json" \
  -d '{"question":"whale migration patterns"}'
```
Expected: Under 2 seconds for 5M chunks

### Step 4.2: Test multiple rapid queries
```bash
for i in {1..10}; do
  curl -s -X POST http://127.0.0.1:8888/chat \
    -H "Content-Type: application/json" \
    -d '{"question":"test query '$i'"}' &
done
wait
```
Expected: All complete without errors

### Step 4.3: Run full accuracy test
```bash
python3 << 'EOF'
import urllib.request
import json

questions = [
    ("Who is Tony Balay", ["bbq", "champion", "grill"]),
    ("BBQ class Missoula cost", ["35", "dollar"]),
    ("Carbon Copy Cloner", ["mac", "clone", "drive"]),
    ("Denver school bond", ["572", "million"]),
    ("whale migration", ["ocean", "pacific", "breeding"]),
    ("black hat SEO", ["spam", "penalty", "google"]),
    ("photosynthesis", ["plant", "light", "chlorophyll"]),
    ("machine learning", ["algorithm", "model", "data"]),
]

passed = 0
for q, expected in questions:
    try:
        data = json.dumps({"question": q}).encode()
        req = urllib.request.Request(
            "http://127.0.0.1:8888/chat",
            data=data,
            headers={"Content-Type": "application/json"}
        )
        resp = urllib.request.urlopen(req, timeout=30)
        result = json.loads(resp.read().decode())
        answer = result.get("response", "").lower()
        if any(e in answer for e in expected):
            print(f"[PASS] {q}")
            passed += 1
        else:
            print(f"[FAIL] {q} -> {answer[:100]}")
    except Exception as e:
        print(f"[ERROR] {q} -> {e}")

print(f"\nResult: {passed}/{len(questions)} passed ({100*passed//len(questions)}%)")
EOF
```

---

## Success Criteria

- [ ] Single curl POST returns valid JSON response (not "Invalid request")
- [ ] Server logs show full request body received
- [ ] 80%+ accuracy on test questions
- [ ] Response time under 2 seconds per query
- [ ] No crashes under rapid queries
- [ ] Demo HTML page works

---

## Rollback Plan

If fix doesn't work, restore original:
```bash
# The original is still at:
cp "/media/yeblad/Ocean Eterna/Ocean/main/src/chat/ocean_chat_server.cpp" \
   /home/yeblad/OE_1.24.26/chat/ocean_chat_server.cpp
```

---

## Files Reference

| File | Purpose |
|------|---------|
| `/home/yeblad/OE_1.24.26/chat/ocean_chat_server.cpp` | Main server code |
| `/home/yeblad/OE_1.24.26/chat/ocean_demo.html` | Browser test page |
| `/tmp/test_ocean.py` | Python test script |
| `/tmp/server_out.txt` | Server logs |
| `/home/yeblad/OE_1.24.26/chat/guten_9m_build/` | Index files |
