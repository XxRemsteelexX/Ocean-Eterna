# Ocean Eterna — PicoClaw Skill

## Description
Ocean Eterna is a BM25 search engine for personal knowledge. Sub-10ms search
across 1B+ tokens. This skill wraps the OE REST API as a shell script — no
Python or MCP dependencies required.

## Prerequisites
- Ocean Eterna server running on `http://localhost:9090`
- `curl` and `bash` (standard on any system PicoClaw runs on)
- `python3` only needed for JSON-safe string escaping in a few commands

## Installation

Copy the skill wrapper to your PicoClaw skills directory:

```bash
cp /home/yeblad/Ocean-Eterna/integrations/picoclaw/oe_skill.sh ~/.picoclaw/skills/
chmod +x ~/.picoclaw/skills/oe_skill.sh
```

Or set a custom OE server URL:

```bash
export OE_BASE_URL="http://192.168.1.100:9090"
```

## Commands

### search — query the knowledge base
```bash
oe_skill.sh search "machine learning gradient descent"
```
Returns JSON with `answer`, `sources`, `search_time_ms`, and `turn_id`.

### stats — server statistics
```bash
oe_skill.sh stats
```
Returns chunk count, token count, DB size, unique sources.

### health — server health check
```bash
oe_skill.sh health
```
Returns version, status, creature tier, tentacle count.

### get-chunk — retrieve a specific chunk
```bash
oe_skill.sh get-chunk DOC-42
oe_skill.sh get-chunk DOC-42 3    # with context_window=3
```

### add-file — ingest text content
```bash
oe_skill.sh add-file "meeting_notes.txt" "contents of the meeting..."
```

### add-file-path — ingest a file from disk
```bash
oe_skill.sh add-file-path /home/user/documents/notes.txt
```

### add-document — ingest any supported format
```bash
oe_skill.sh add-document /home/user/paper.pdf
```
Note: server-side document processing handles PDF, DOCX, XLSX, CSV, images.

### catalog — browse indexed chunks
```bash
oe_skill.sh catalog           # page 1, 20 per page
oe_skill.sh catalog 2 50      # page 2, 50 per page
```

### tell-me-more — expand a previous search
```bash
oe_skill.sh tell-me-more "turn_abc123"
```

### reconstruct — combine chunks into a document
```bash
oe_skill.sh reconstruct DOC-1 DOC-2 DOC-3
```

## When to Use

- **User asks about their notes/documents** -> `search`
- **Need more context** -> `get-chunk` with higher context_window, or `tell-me-more`
- **User wants to add content** -> `add-file`, `add-file-path`, or `add-document`
- **Explore what's indexed** -> `catalog` or `stats`
- **Read a full section** -> `reconstruct` with chunk IDs from search results

## Output Format

All commands return raw JSON from the OE server. Parse with `jq` if needed:

```bash
oe_skill.sh search "transformers" | jq '.answer'
oe_skill.sh stats | jq '.chunks_loaded'
```
