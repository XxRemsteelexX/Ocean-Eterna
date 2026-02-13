# Ocean Eterna: Technical Specification

## System Classification
Ocean Eterna is a **high-performance Retrieval-Augmented Generation (RAG) system** optimized for CPU-only execution, portable deployment, and infinite effective context through chunk ID referencing.

---

## Core Architecture

### Data Pipeline
```
Raw Text → Tokenization → Adaptive Chunking → LZ4 Compression → Binary Storage
                ↓
         BM25 Inverted Index → In-Memory Search Structure
                ↓
         Manifest (JSONL) → Chunk Metadata + Offsets
```

### Storage Format

**Binary Storage (`*.bin`):**
- LZ4-compressed chunks stored contiguously
- Each chunk: `[4-byte length][LZ4 compressed content]`
- Compression ratio: ~4:1 on prose text
- Build throughput: 34+ million tokens/second

**Manifest (`manifest_*.jsonl`):**
```json
{"chunk_id":"guten9m_DOC_42","offset":1847293,"length":2048,"tokens":512,"type":"DOC","terms":{"whale":7,"ship":3,"captain":2}}
```
- One JSON object per line
- Contains: chunk_id, byte offset, compressed length, token count, content type, term frequencies
- Enables O(1) chunk retrieval via offset seeking

**Chapter Guide (`chapter_guide.json`):**
```json
{
  "version": "3.0",
  "chunks": {"total": 22577, "by_type": {"DOC": 22576, "CHAT": 1, "CODE": 0}},
  "code_files": {"count": 0, "files": []},
  "conversations": {"count": 1, "summaries": []},
  "fixes": {"count": 0, "entries": []},
  "features": {"count": 0, "entries": []}
}
```
- Navigation structure for cross-referencing conversations, code, fixes, features
- Updated at runtime as conversations occur

---

## Search Algorithm

### BM25 Implementation
```
score(D,Q) = Σ IDF(qi) * (f(qi,D) * (k1 + 1)) / (f(qi,D) + k1 * (1 - b + b * |D|/avgdl))
```
- **k1 = 1.2** (term frequency saturation)
- **b = 0.75** (document length normalization)
- Inverted index: `unordered_map<string, vector<pair<uint32_t, float>>>` (term → [(chunk_index, tf)])
- IDF precomputed at load time
- Search time: **~40ms on 9M tokens** (22K chunks)

### Index Structure
```cpp
struct SearchIndex {
    vector<ChunkOut> chunks;                           // All chunk metadata
    unordered_map<string, vector<pair<uint32_t, float>>> inverted_index;  // term → occurrences
    unordered_map<string, float> idf;                  // precomputed IDF values
    unordered_map<string, uint32_t> chunk_id_to_index; // O(1) ID lookup
    float avg_doc_length;
};
```

---

## Chunk ID System

### Format
```
{corpus_code}_{TYPE}_{index}
```
- **corpus_code**: Short identifier (e.g., `guten9m`, `myapp`)
- **TYPE**: One of `DOC`, `CHAT`, `CODE`, `FIX`, `FEAT`
- **index**: Sequential integer within type

### Examples
```
guten9m_DOC_42      # Document chunk from Gutenberg corpus
myapp_CODE_15       # Code chunk from application
myapp_CHAT_3        # Conversation turn
myapp_FIX_1         # Bug fix record
myapp_FEAT_2        # Feature implementation
```

### Compression Ratio
- Average chunk: ~500 tokens (~2000 characters)
- Chunk ID: ~15 characters
- **Compression: 133:1 to 150:1**
- Enables referencing unlimited context within LLM token limits

---

## Content Type Detection

### Algorithm
```cpp
ContentType detect_content_type(const string& text, const string& filename) {
    // 1. Check filename extension
    if (ends_with(filename, {".cpp", ".py", ".js", ".ts", ".go", ".rs"})) return CODE;
    if (ends_with(filename, {".md", ".txt", ".pdf"})) return DOCUMENT;

    // 2. Check content patterns
    if (contains(text, {"```", "def ", "function ", "class ", "#include"})) return CODE;
    if (contains(text, {"User:", "Assistant:", "Human:", "AI:"})) return CHAT;
    if (contains(text, {"fix", "bug", "patch", "issue #"})) return FIX;
    if (contains(text, {"feature", "implement", "add support"})) return FEATURE;

    return DOCUMENT;  // default
}
```

### Adaptive Boundaries
- **Documents**: Split at paragraph/sentence boundaries (`. `, `\n\n`)
- **Code**: Split at function/class boundaries (`}\n`, `def `, `class `)
- **Target**: 500 tokens ±20% to maintain semantic coherence

---

## Conversation-Chunk Linking

### Data Structure
```cpp
struct ChunkReference {
    string chunk_id;       // e.g., "guten9m_DOC_42"
    double relevance_score; // BM25 score
    string snippet;        // First 200 chars
};

struct ConversationTurn {
    string turn_id;                    // e.g., "myapp_CHAT_5"
    string question;
    string answer;
    vector<ChunkReference> source_refs; // Chunks used to generate answer
    double search_time_ms;
    double llm_time_ms;
};
```

### "Tell Me More" Feature
```cpp
// No re-search: reuses cached source_refs from previous turn
unordered_map<string, ConversationTurn> recent_turns_cache;

json handle_tell_me_more(const string& prev_turn_id, const string& aspect) {
    auto& prev = recent_turns_cache[prev_turn_id];
    // Reconstruct context from prev.source_refs without BM25 search
    string context = reconstruct_context_from_ids(prev.source_refs);
    // Generate new response with same sources
}
```

---

## HTTP API

| Endpoint | Method | Function |
|----------|--------|----------|
| `/chat` | POST | `{question, top_k}` → `{answer, sources[], search_ms, llm_ms}` |
| `/stats` | GET | System metrics (tokens, chunks, CPU, RAM) |
| `/guide` | GET | Full chapter guide JSON |
| `/chunk/{id}` | GET | Retrieve chunk content by ID |
| `/tell-me-more` | POST | Continue with cached sources (0ms search) |
| `/sources` | POST | Get sources for a turn_id |
| `/reconstruct` | POST | Rebuild context from chunk IDs |

---

## Performance Characteristics

| Metric | Value | Notes |
|--------|-------|-------|
| Build speed | 34M tokens/sec | Single-threaded LZ4 compression |
| Load time | 150-200ms | 9M tokens, 22K chunks |
| Search time | 40ms | BM25 on 22K chunks |
| Chunk lookup | 0.02ms | O(1) hash table |
| Compression | 147:1 | Chunk IDs vs full content |
| Memory | ~500MB | For 9M token corpus |

---

## Key Innovations

1. **Chunk ID References**: Responses include `📎 Sources: guten9m_DOC_42, guten9m_DOC_108` enabling 147:1 context compression. Future queries can reference these IDs to reconstruct full context.

2. **Zero-Search Continuation**: "Tell me more" uses cached `source_refs` from previous turn, achieving 0ms search time for follow-up questions.

3. **Portable Execution**: Runs from USB drive, CPU-only, no GPU required. Static compilation option for zero-dependency deployment.

4. **Unified Type System**: All content (documents, conversations, code, fixes, features) shares the same chunk format and search infrastructure, enabling cross-referencing.

5. **Semantic Chunking**: Content-aware boundary detection preserves meaning across chunk splits.

---

## File Structure
```
OE_1.24.26/
├── chat/
│   ├── ocean_chat_server          # HTTP server binary
│   ├── ocean_demo.html            # Web interface
│   └── guten_9m_build/
│       ├── manifest_guten9m.jsonl # Chunk metadata
│       ├── chapter_guide.json     # Navigation structure
│       └── storage/guten9m.bin    # Compressed chunks
├── core/
│   └── ocean_build                # Index builder binary
└── test_data/
    └── *.txt                      # Source documents
```

---

## Usage Pattern for LLM Integration

```python
# 1. Search for relevant chunks
response = POST("/chat", {"question": "How does authentication work?", "top_k": 5})
# Returns: answer + source_refs[{chunk_id, score, snippet}]

# 2. Store chunk IDs in conversation context (not full text)
context = "Previous sources: " + ", ".join([r.chunk_id for r in response.source_refs])

# 3. Later: reconstruct full context from IDs
full_context = POST("/reconstruct", {"chunk_ids": ["myapp_CODE_15", "myapp_DOC_42"]})

# 4. Follow-up without re-search
more = POST("/tell-me-more", {"prev_turn_id": response.turn_id, "aspect": "error handling"})
```

This enables infinite effective context: store chunk IDs (15 chars each) instead of full text (2000 chars each), reconstruct on demand.
