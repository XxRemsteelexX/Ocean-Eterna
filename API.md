# OceanEterna v4 API Reference

Base URL: `http://localhost:8888` (configurable via config.json)

## Health & Status

### GET /health
Returns server health status. Bypasses auth and rate limiting.

**Response:**
```json
{"status": "ok", "version": "4.0"}
```

### GET /stats
Returns server statistics.

**Response:**
```json
{
  "chunks_loaded": 5065226,
  "total_tokens": 2457306692,
  "total_queries": 42,
  "avg_search_ms": 35.2,
  "avg_llm_ms": 1200.5,
  "db_size_mb": 7040.1,
  "ram_usage": 0.48,
  "ram_used_gb": 7.5,
  "ram_total_gb": 15.6,
  "cpu_usage": 0.12
}
```

## Chat & Search

### POST /chat
Main chat endpoint. Searches corpus, builds context, queries LLM.

**Request:**
```json
{"question": "What is photosynthesis?"}
```

**Response:**
```json
{
  "answer": "Photosynthesis is the process by which...",
  "formatted_answer": "<p>Photosynthesis is...</p>",
  "turn_id": "CH42",
  "search_time_ms": 35,
  "llm_time_ms": 1200,
  "total_time_ms": 1235,
  "chunks_retrieved": 8,
  "compression_ratio": 667.5,
  "sources": []
}
```

### POST /chat/stream (v4.1)
SSE streaming endpoint. Same functionality as `/chat` but returns Server-Sent Events for progressive token display.

**Request:**
```json
{"question": "What is photosynthesis?"}
```

**Response:** Server-Sent Events stream with three event types:

```
event: search
data: {"search_time_ms": 35, "chunks_retrieved": 8}

data: {"token": "Photo"}
data: {"token": "synthesis"}
data: {"token": " is"}
...

event: done
data: {"turn_id": "CH42", "total_time_ms": 1235, "llm_time_ms": 1200, "sources": [...]}
```

**Event Types:**
- `search` - Sent immediately after search completes (before LLM)
- (no event type) - Token data events as LLM generates response
- `done` - Final metadata including turn_id and sources

**Headers:**
```
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
```

### POST /sources
Get source references for a previous chat turn.

**Request:**
```json
{"turn_id": "CH42"}
```

**Response:**
```json
{
  "success": true,
  "turn_id": "CH42",
  "source_count": 8,
  "sources": [
    {"chunk_id": "guten9m.1234", "score": 5.2, "snippet": "..."}
  ]
}
```

### POST /tell-me-more
Expand on a previous answer using cached source context.

**Request:**
```json
{"prev_turn_id": "CH42"}
```

**Response:**
```json
{
  "answer": "Additionally, photosynthesis...",
  "turn_id": "CH43",
  "search_time_ms": 0,
  "llm_time_ms": 1100
}
```

## Chunk Operations

### GET /chunk/:id
Retrieve a specific chunk by ID. Decompresses from storage (LZ4 or Zstd).

**Example:** `GET /chunk/guten9m.1234`

**Response:**
```json
{
  "success": true,
  "chunk_id": "guten9m.1234",
  "content": "The text content of this chunk..."
}
```

### POST /reconstruct
Reconstruct text from multiple chunk IDs.

**Request:**
```json
{"chunk_ids": ["guten9m.1", "guten9m.2", "guten9m.3"]}
```

**Response:**
```json
{
  "success": true,
  "chunk_ids": ["guten9m.1", "guten9m.2", "guten9m.3"],
  "context": "Combined text from all chunks...",
  "context_length": 6000,
  "compression_ratio": 500.0
}
```

### POST /extract-ids
Extract chunk IDs from text.

**Request:**
```json
{"text": "See chunks guten9m.1 and guten9m.2 for details"}
```

**Response:**
```json
{"chunk_ids": ["guten9m.1", "guten9m.2"]}
```

## Specialized Queries

### POST /query/code-discussions
Search for code discussion chunks.

### POST /query/fixes
Search for bug fix and solution chunks.

### POST /query/feature
Search for feature request and enhancement chunks.

All three use the same request/response format as `/chat`.

## Content Management

### POST /add-file
Add a file to the search index. Content is chunked, compressed (Zstd), and indexed.

**Request:**
```json
{
  "filename": "document.txt",
  "content": "The full text content of the file..."
}
```

**Response:**
```json
{
  "success": true,
  "filename": "document.txt",
  "chunks_added": 3,
  "tokens_added": 1500,
  "total_chunks": 5065229,
  "total_tokens": 2457308192
}
```

### POST /add-file-path
Add a file from the server filesystem.

**Request:**
```json
{"file_path": "/path/to/document.txt"}
```

### POST /clear-database
Clear conversation history (not the corpus).

**Response:**
```json
{"success": true, "message": "Conversation history cleared"}
```

## Reference

### GET /guide
Returns the chapter guide (book index for the corpus).

**Response:** JSON object with chapter structure, chunk ID format, and features.

## CORS

All endpoints support CORS. OPTIONS preflight requests return 204 with:
- `Access-Control-Allow-Origin: *`
- `Access-Control-Allow-Methods: GET, POST, OPTIONS`
- `Access-Control-Allow-Headers: Content-Type, Authorization, X-API-Key`

## Authentication (Optional)

When `auth.enabled = true` in config.json:
- All requests (except `/health`) require `X-API-Key` header
- Returns 401 if key is missing or incorrect
- Set key via config.json `auth.api_key` or env `OCEAN_SERVER_API_KEY`

## Rate Limiting (Optional)

When `rate_limit.enabled = true` in config.json:
- Per-IP token bucket algorithm
- Configurable `requests_per_minute` (default: 60)
- Returns 429 when rate limit exceeded
- `/health` endpoint bypasses rate limiting
