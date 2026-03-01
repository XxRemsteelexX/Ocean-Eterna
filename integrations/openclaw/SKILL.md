# Ocean Eterna — OpenClaw Skill

## Description
Ocean Eterna is a BM25 search engine for personal knowledge — notes, documents,
conversations, transcripts. Sub-10ms search across 1B+ tokens.

## Prerequisites
- Ocean Eterna server running on `http://localhost:9090`
- `mcporter` CLI installed (`pip install mcporter`)
- Python 3.10+ with `fastmcp` and `requests`

## Setup

### Option A: mcporter bridge (works today)

Register the MCP server with mcporter:

```bash
mcporter register --config /home/yeblad/Ocean-Eterna/integrations/openclaw/mcporter.json
```

Then in your OpenClaw agent, call tools via mcporter:

```bash
mcporter call ocean-eterna oe_search '{"query": "machine learning basics"}'
mcporter call ocean-eterna oe_stats '{}'
mcporter call ocean-eterna oe_add_document '{"path": "/path/to/file.pdf"}'
```

### Option B: native MCP (when supported)

Add to `~/.openclaw/openclaw.json`:

```json
{
  "mcpServers": {
    "ocean-eterna": {
      "command": "python3",
      "args": ["/home/yeblad/Ocean-Eterna/mcp_server.py"],
      "env": { "OE_BASE_URL": "http://localhost:9090" }
    }
  }
}
```

## Available Tools

| Tool | Description | Example Args |
|------|-------------|-------------|
| `oe_search` | BM25 search across knowledge base | `{"query": "neural networks"}` |
| `oe_get_chunk` | retrieve chunk by ID + neighbors | `{"chunk_id": "DOC-42", "context_window": 2}` |
| `oe_add_file` | ingest text content | `{"filename": "notes.txt", "content": "..."}` |
| `oe_add_file_path` | ingest file from disk path | `{"path": "/home/user/doc.txt"}` |
| `oe_add_document` | ingest PDF/DOCX/XLSX/CSV/image | `{"path": "/home/user/paper.pdf"}` |
| `oe_stats` | server health + corpus stats | `{}` |
| `oe_catalog` | browse indexed chunks | `{"page": 1, "page_size": 20}` |
| `oe_tell_me_more` | expand previous search results | `{"turn_id": "abc123"}` |
| `oe_reconstruct` | combine chunks into document | `{"chunk_ids": ["DOC-1", "DOC-2"]}` |

## When to Use

- **User asks about their own notes/documents** -> `oe_search`
- **Need more context around a result** -> `oe_get_chunk` or `oe_tell_me_more`
- **User wants to add content** -> `oe_add_file` (text) or `oe_add_document` (any format)
- **Check what's indexed** -> `oe_catalog` or `oe_stats`
- **Read a full section** -> `oe_reconstruct` with chunk IDs from search results

## Tips

- BM25 matches vocabulary, not semantics — use the user's own words when searching
- Use `context_window=2` or `3` with `oe_get_chunk` for broader context
- After `oe_search`, save the `turn_id` and use `oe_tell_me_more` to dig deeper
- `oe_add_document` handles PDF, DOCX, XLSX, CSV, PNG, JPG with automatic text extraction
