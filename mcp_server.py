#!/usr/bin/env python3
"""Ocean Eterna MCP Server — exposes OE's search engine as MCP tools.

Works with Claude Code, OpenClaw, Agent Zero, and any MCP-compatible client.
Wraps the OE REST API (default http://localhost:9090) as MCP tools over stdio.

Usage:
    python3 mcp_server.py                          # default: localhost:9090
    OE_BASE_URL=http://host:port python3 mcp_server.py  # custom server
"""
import os
import json
import requests
from fastmcp import FastMCP

try:
    from doc_processor import process_document, SUPPORTED_EXTENSIONS
    HAS_DOC_PROCESSOR = True
except ImportError:
    HAS_DOC_PROCESSOR = False

OE_BASE = os.environ.get("OE_BASE_URL", "http://localhost:9090")

mcp = FastMCP(
    "Ocean Eterna",
    instructions=(
        "Ocean Eterna is a BM25 search engine for personal knowledge. "
        "Use oe_search to find information in the user's indexed documents. "
        "Use oe_add_file to ingest new content. Use oe_stats to check corpus size."
    ),
)


def _post(path: str, payload: dict, timeout: int = 120) -> dict:
    """POST to OE server, return JSON or error dict."""
    try:
        r = requests.post(f"{OE_BASE}{path}", json=payload, timeout=timeout)
        return r.json()
    except Exception as e:
        return {"error": str(e)}


def _get(path: str, params: dict = None, timeout: int = 30) -> dict:
    """GET from OE server, return JSON or error dict."""
    try:
        r = requests.get(f"{OE_BASE}{path}", params=params, timeout=timeout)
        return r.json()
    except Exception as e:
        return {"error": str(e)}


# ── Tools ────────────────────────────────────────────────────────────

@mcp.tool()
def oe_search(query: str, conversation_id: str = "") -> str:
    """Search the user's personal knowledge base using BM25.

    Returns the LLM-generated answer plus source chunks with scores.
    Use this whenever the user asks about their own notes, documents,
    conversations, or any previously indexed content.

    Args:
        query: Natural language search query.
        conversation_id: Optional conversation ID for multi-turn context.
    """
    payload = {"question": query}
    if conversation_id:
        payload["conversation_id"] = conversation_id

    data = _post("/chat", payload)

    if "error" in data and "answer" not in data:
        return f"Search error: {data['error']}"

    parts = []

    # answer
    answer = data.get("answer", "")
    if answer:
        parts.append(f"**Answer:**\n{answer}")

    # metadata
    meta = []
    if "search_time_ms" in data:
        meta.append(f"search: {data['search_time_ms']:.0f}ms")
    if "chunks_retrieved" in data:
        meta.append(f"chunks: {data['chunks_retrieved']}")
    if "creature_tier" in data:
        meta.append(f"tier: {data['creature_tier']}")
    if meta:
        parts.append(f"_({', '.join(meta)})_")

    # sources
    sources = data.get("sources", [])
    if sources:
        src_lines = ["**Sources:**"]
        for s in sources[:10]:
            cid = s.get("chunk_id", "?")
            score = s.get("score", 0)
            src_lines.append(f"- `{cid}` (score: {score:.2f})")
        parts.append("\n".join(src_lines))

    # conversation tracking
    if "turn_id" in data:
        parts.append(f"_turn_id: {data['turn_id']}_")

    return "\n\n".join(parts) if parts else json.dumps(data, indent=2)


@mcp.tool()
def oe_get_chunk(chunk_id: str, context_window: int = 1) -> str:
    """Retrieve a specific chunk by ID with optional adjacent chunks.

    Use this to get the full text of a source chunk found by oe_search,
    or to expand context around a relevant chunk.

    Args:
        chunk_id: The chunk identifier (from search results).
        context_window: Number of adjacent chunks to include (0-3). Default 1.
    """
    data = _get(f"/chunk/{chunk_id}", {"context_window": context_window})

    if data.get("success") is False:
        return f"Chunk not found: {chunk_id}"

    parts = []
    parts.append(f"**Chunk {chunk_id}:**\n{data.get('content', '')}")

    # metadata
    meta = {}
    for key in ["source_file", "type", "category", "tokens", "prev_chunk_id", "next_chunk_id"]:
        if key in data and data[key]:
            meta[key] = data[key]
    if meta:
        parts.append("**Metadata:** " + ", ".join(f"{k}: {v}" for k, v in meta.items()))

    # adjacent chunks
    adj = data.get("adjacent_chunks", [])
    if adj:
        for a in adj:
            parts.append(f"**Adjacent ({a.get('chunk_id', '?')}):**\n{a.get('content', '')[:500]}")

    return "\n\n".join(parts)


@mcp.tool()
def oe_add_file(filename: str, content: str) -> str:
    """Ingest a new document into the Ocean Eterna knowledge base.

    The content will be chunked using paragraph-safe boundaries and indexed
    for BM25 search. Use this to add notes, documents, or any text content.

    Args:
        filename: Name for the document (e.g. "meeting_notes.txt").
        content: The full text content to index.
    """
    data = _post("/add-file", {"filename": filename, "content": content})

    if data.get("success"):
        return (
            f"Ingested **{filename}**: "
            f"{data.get('chunks_added', 0)} chunks, "
            f"{data.get('tokens_added', 0)} tokens"
        )
    return f"Ingestion failed: {data.get('error', json.dumps(data))}"


@mcp.tool()
def oe_add_file_path(path: str) -> str:
    """Ingest a file from the local filesystem into the knowledge base.

    The file at the given path will be read, chunked, and indexed.
    Path must be within the server's allowed directory.

    Args:
        path: Absolute or relative path to the file to ingest.
    """
    data = _post("/add-file-path", {"path": path})

    if data.get("success"):
        return (
            f"Ingested **{path}**: "
            f"{data.get('chunks_added', 0)} chunks, "
            f"{data.get('tokens_added', 0)} tokens"
        )
    return f"Ingestion failed: {data.get('error', json.dumps(data))}"


@mcp.tool()
def oe_stats() -> str:
    """Get Ocean Eterna server statistics.

    Returns corpus size, chunk count, token count, creature tier,
    and other server health information.
    """
    health = _get("/health")
    stats = _get("/stats")

    parts = []
    parts.append(f"**Ocean Eterna v{health.get('version', '?')}** — {health.get('status', '?')}")

    if "creature_tier" in health:
        parts.append(f"Creature Tier: **{health['creature_tier']}** ({health.get('tentacles', '?')} tentacles)")

    stat_lines = []
    for key in ["chunks_loaded", "total_tokens", "db_size_mb", "unique_sources", "conversations"]:
        if key in stats:
            val = stats[key]
            if isinstance(val, (int, float)) and val > 999:
                stat_lines.append(f"- {key}: {val:,}")
            else:
                stat_lines.append(f"- {key}: {val}")
    if stat_lines:
        parts.append("\n".join(stat_lines))

    return "\n\n".join(parts)


@mcp.tool()
def oe_catalog(page: int = 1, page_size: int = 20, type_filter: str = "", source: str = "") -> str:
    """Browse the indexed knowledge base catalog.

    Lists chunks with metadata, supports filtering by type and source file.
    Use this to explore what's been indexed.

    Args:
        page: Page number (default 1).
        page_size: Results per page (default 20, max 100).
        type_filter: Filter by chunk type (e.g. "DOC", "CH").
        source: Filter by source filename.
    """
    params = {"page": page, "page_size": min(page_size, 100)}
    if type_filter:
        params["type"] = type_filter
    if source:
        params["source"] = source

    data = _get("/catalog", params)

    parts = []
    parts.append(f"**Catalog** — {data.get('total_chunks', 0):,} total chunks")

    types = data.get("types", {})
    if types:
        parts.append("**Types:** " + ", ".join(f"{k}: {v:,}" for k, v in types.items()))

    sources = data.get("source_files", [])
    if sources:
        parts.append(f"**Sources ({len(sources)}):** " + ", ".join(sources[:20]))

    chunks = data.get("chunks", [])
    if chunks:
        chunk_lines = [f"**Chunks (page {page}):**"]
        for c in chunks:
            chunk_lines.append(
                f"- `{c.get('chunk_id', '?')}` | {c.get('type', '?')} | "
                f"{c.get('tokens', 0)} tokens | {c.get('source_file', '?')}"
            )
        parts.append("\n".join(chunk_lines))

    return "\n\n".join(parts)


@mcp.tool()
def oe_tell_me_more(turn_id: str) -> str:
    """Expand context for a previous search result.

    After a search, use this to get more information by expanding the
    context window around the previously retrieved chunks.

    Args:
        turn_id: The turn_id from a previous oe_search result.
    """
    data = _post("/tell-me-more", {"turn_id": turn_id})

    if "error" in data and "answer" not in data:
        return f"Error: {data['error']}"

    parts = []
    answer = data.get("answer", "")
    if answer:
        parts.append(f"**Expanded Answer:**\n{answer}")

    sources = data.get("sources", [])
    if sources:
        parts.append(f"_({len(sources)} sources, window expanded)_")

    return "\n\n".join(parts) if parts else json.dumps(data, indent=2)


@mcp.tool()
def oe_add_document(path: str) -> str:
    """Ingest a document in any supported format into Ocean Eterna.

    Supports: PDF, DOCX, XLSX, CSV, PNG, JPG, TXT, MD.
    The document is preprocessed (text extracted, OCR if image),
    normalized to clean paragraphs, and sent to the search index.

    Args:
        path: Absolute path to the document file.
    """
    if not HAS_DOC_PROCESSOR:
        return "Error: doc_processor not available. Install: pip install pymupdf pytesseract"

    try:
        filename, content = process_document(path)
    except Exception as e:
        return f"Preprocessing failed: {e}"

    if not content.strip():
        return f"No text content extracted from {path}"

    data = _post("/add-file", {"filename": filename, "content": content})

    if data.get("success"):
        return (
            f"Ingested **{filename}**: "
            f"{data.get('chunks_added', 0)} chunks, "
            f"{data.get('tokens_added', 0)} tokens "
            f"({len(content):,} chars extracted)"
        )
    return f"Ingestion failed: {data.get('error', json.dumps(data))}"


@mcp.tool()
def oe_reconstruct(chunk_ids: list[str]) -> str:
    """Reconstruct a document from a list of chunk IDs.

    Combines multiple chunks back into a continuous document.
    Useful for reading full sections or documents.

    Args:
        chunk_ids: List of chunk IDs to combine in order.
    """
    data = _post("/reconstruct", {"chunk_ids": chunk_ids})

    if data.get("success"):
        return f"**Reconstructed ({len(chunk_ids)} chunks):**\n\n{data.get('content', '')}"
    return f"Reconstruction failed: {data.get('error', json.dumps(data))}"


# ── Resources ────────────────────────────────────────────────────────

@mcp.resource("oe://stats")
def resource_stats() -> str:
    """Current Ocean Eterna server statistics."""
    return oe_stats()


@mcp.resource("oe://guide")
def resource_guide() -> str:
    """Quick guide for using Ocean Eterna."""
    return """# Ocean Eterna Quick Guide

## What is it?
A BM25 search engine for your personal knowledge — conversations, documents, notes.

## Tools:
- **oe_search(query)** — Search your knowledge base. Start here.
- **oe_get_chunk(chunk_id)** — Get full text of a specific chunk.
- **oe_add_file(filename, content)** — Add new content to the index.
- **oe_add_file_path(path)** — Index a plaintext file from disk.
- **oe_add_document(path)** — Ingest PDF, DOCX, XLSX, CSV, or image (OCR).
- **oe_stats()** — Check server health and corpus size.
- **oe_catalog()** — Browse what's indexed.
- **oe_tell_me_more(turn_id)** — Expand previous search results.
- **oe_reconstruct(chunk_ids)** — Combine chunks into full document.

## Tips:
- Search with your own words — BM25 matches vocabulary, not semantics.
- Use oe_get_chunk with context_window=2 or 3 for broader context.
- After oe_search, use turn_id with oe_tell_me_more to dig deeper.
- Use oe_add_document for any file type — it handles conversion automatically.
"""


if __name__ == "__main__":
    mcp.run(transport="stdio")
