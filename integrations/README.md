# Ocean Eterna — Universal Claw Integrations

One MCP server, many consumers. These configs and wrappers let any major Claw
framework use Ocean Eterna's BM25 search engine.

## What is Ocean Eterna?

A BM25 search engine for personal knowledge — notes, documents, transcripts,
conversations. Sub-10ms search across 1B+ tokens with content-aware chunking.

**9 tools available:**

| Tool | What it does |
|------|-------------|
| `oe_search` | BM25 search across your knowledge base |
| `oe_get_chunk` | retrieve a chunk by ID + neighbors |
| `oe_add_file` | ingest text content |
| `oe_add_file_path` | ingest a file from disk |
| `oe_add_document` | ingest PDF, DOCX, XLSX, CSV, or image (OCR) |
| `oe_stats` | server health and corpus statistics |
| `oe_catalog` | browse indexed chunks |
| `oe_tell_me_more` | expand previous search results |
| `oe_reconstruct` | combine chunks into a continuous document |

## Prerequisites

1. Ocean Eterna server running (default: `http://localhost:9090`)
2. Python 3.10+ with `fastmcp` and `requests` (for MCP-based integrations)
3. The MCP server: `mcp_server.py` in the repo root

Start the OE server, then pick your framework below.

---

## Framework Setup

### Claude Code (already configured)

```bash
claude mcp add ocean-eterna -- python3 /home/yeblad/Ocean-Eterna/mcp_server.py
```

Tools appear automatically. No config files needed.

---

### OpenClaw

OpenClaw uses `mcporter` as an MCP bridge until native support lands.

**Option A — mcporter (works today):**

```bash
mcporter register --config /home/yeblad/Ocean-Eterna/integrations/openclaw/mcporter.json
```

Call tools via mcporter:
```bash
mcporter call ocean-eterna oe_search '{"query": "neural networks"}'
```

**Option B — native MCP (future):**

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

See `openclaw/SKILL.md` for full tool reference and usage guidance.

---

### Agent Zero

Agent Zero has native MCP support. Paste the config into Agent Zero's settings UI
or merge into `usr/settings.json`:

```json
{
  "mcpServers": {
    "ocean-eterna": {
      "description": "Ocean Eterna BM25 search engine — 9ms search across 1B+ tokens",
      "command": "python3",
      "args": ["/home/yeblad/Ocean-Eterna/mcp_server.py"],
      "env": { "OE_BASE_URL": "http://localhost:9090" }
    }
  }
}
```

Tools auto-discover and namespace as `ocean_eterna.oe_search`,
`ocean_eterna.oe_add_document`, etc.

Config file: `agent-zero/mcp_config.json`

---

### IronClaw

IronClaw (Rust, WASM sandboxed) has native MCP since v0.10.0.

**CLI registration:**
```bash
ironclaw mcp add ocean-eterna --command "python3 /home/yeblad/Ocean-Eterna/mcp_server.py"
```

**Or use the config file** — merge `ironclaw/mcp_config.json` into your IronClaw
settings:

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

---

### PicoClaw

PicoClaw explicitly rejected MCP. This integration uses a shell script that calls
the OE REST API directly via `curl` — no Python or MCP dependencies.

**Install the skill:**
```bash
cp integrations/picoclaw/oe_skill.sh ~/.picoclaw/skills/
chmod +x ~/.picoclaw/skills/oe_skill.sh
```

**Usage:**
```bash
oe_skill.sh search "gradient descent"
oe_skill.sh stats
oe_skill.sh add-document /path/to/paper.pdf
oe_skill.sh get-chunk DOC-42 2
oe_skill.sh catalog 1 50
```

See `picoclaw/SKILL.md` for the full command reference.

---

### TinyClaw

TinyClaw (TypeScript/Bun, pre-release) has no MCP support. This integration is a
native tool plugin that calls the OE REST API directly via `fetch`.

**Register the plugin:**
```typescript
import oePlugin from "./integrations/tinyclaw/oe_tool.ts";

// register with tinyclaw
agent.registerPlugin(oePlugin);
```

All 9 tools are exported with proper parameter schemas. The plugin exposes:
- `oe_search`, `oe_get_chunk`, `oe_add_file`, `oe_add_file_path`,
  `oe_add_document`, `oe_stats`, `oe_catalog`, `oe_tell_me_more`,
  `oe_reconstruct`

See `tinyclaw/oe_tool.ts` for the full implementation.

---

## Custom OE Server URL

All integrations default to `http://localhost:9090`. To change:

- **MCP configs:** set `"OE_BASE_URL"` in the `"env"` block
- **Shell script:** `export OE_BASE_URL="http://192.168.1.100:9090"`
- **TypeScript plugin:** `export OE_BASE_URL="http://192.168.1.100:9090"`

## File Structure

```
integrations/
├── README.md                    # this file
├── openclaw/
│   ├── mcporter.json           # mcporter bridge config
│   └── SKILL.md                # skill definition + tool reference
├── agent-zero/
│   └── mcp_config.json         # paste into Agent Zero UI/settings
├── ironclaw/
│   └── mcp_config.json         # native MCP config
├── picoclaw/
│   ├── oe_skill.sh             # REST API shell wrapper (no deps)
│   └── SKILL.md                # PicoClaw skill definition
└── tinyclaw/
    └── oe_tool.ts              # Bun/TypeScript tool plugin
```
