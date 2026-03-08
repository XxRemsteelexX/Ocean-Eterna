#!/usr/bin/env python3
"""oe_auto_ingest — auto-ingest chat content to Ocean Eterna every 25K tokens.

works with any text source: Claude Code conversations, Agent Zero logs,
OpenClaw chats, or arbitrary text files.

usage:
  # ingest a file (tracks token count, only sends when 25K+ new tokens)
  python3 auto_ingest.py --file /path/to/chat.log

  # ingest from stdin (pipe chat content)
  echo "conversation text" | python3 auto_ingest.py --stdin --source "agent_zero"

  # ingest Claude Code current session (parses JSONL transcript)
  python3 auto_ingest.py --claude-code

  # force ingest regardless of token count
  python3 auto_ingest.py --file /path/to/chat.log --force

  # check status
  python3 auto_ingest.py --status
"""

import argparse
import glob
import json
import os
import re
import sys
import time
from pathlib import Path

OE_URL = os.environ.get("OE_URL", "http://localhost:9090")
STATE_FILE = os.path.expanduser("~/.oe_auto_ingest_state.json")
TOKEN_THRESHOLD = int(os.environ.get("OE_TOKEN_THRESHOLD", "25000"))

def count_tokens(text: str) -> int:
    """approximate token count (words * 1.3 for subword tokenization)"""
    return int(len(text.split()) * 1.3)

def load_state() -> dict:
    if os.path.exists(STATE_FILE):
        with open(STATE_FILE) as f:
            return json.load(f)
    return {"sources": {}, "total_ingested_tokens": 0, "last_ingest": None}

def save_state(state: dict):
    with open(STATE_FILE, "w") as f:
        json.dump(state, f, indent=2)

def oe_health() -> bool:
    try:
        import urllib.request
        r = urllib.request.urlopen(f"{OE_URL}/stats", timeout=3)
        return r.status == 200
    except Exception:
        return False

def oe_ingest(filename: str, content: str) -> bool:
    try:
        import urllib.request
        data = json.dumps({"filename": filename, "content": content}).encode()
        req = urllib.request.Request(
            f"{OE_URL}/add-file",
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST"
        )
        r = urllib.request.urlopen(req, timeout=30)
        return r.status == 200
    except Exception as e:
        print(f"[oe_auto_ingest] ingest failed: {e}", file=sys.stderr)
        return False

def extract_claude_code_content() -> tuple[str, str]:
    """extract text from the most recent Claude Code conversation."""
    # find the most recent .jsonl conversation file (exclude subagents)
    claude_dir = os.path.expanduser("~/.claude/projects")
    all_jsonl = glob.glob(f"{claude_dir}/**/*.jsonl", recursive=True)
    jsonl_files = sorted(
        [f for f in all_jsonl if "/subagents/" not in f],
        key=os.path.getmtime,
        reverse=True
    )

    if not jsonl_files:
        return "", ""

    latest = jsonl_files[0]
    session_id = Path(latest).stem

    messages = []
    try:
        with open(latest) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError:
                    continue

                # claude code stores role inside entry.message.role
                msg = entry.get("message", entry)
                role = msg.get("role", "")
                if role not in ("user", "assistant"):
                    continue

                content = msg.get("content", "")
                if isinstance(content, str):
                    text = content
                elif isinstance(content, list):
                    # extract text blocks from content array
                    parts = []
                    for block in content:
                        if isinstance(block, dict):
                            if block.get("type") == "text":
                                parts.append(block.get("text", ""))
                            elif block.get("type") == "tool_result":
                                for sub in block.get("content", []):
                                    if isinstance(sub, dict) and sub.get("type") == "text":
                                        parts.append(sub.get("text", "")[:500])
                        elif isinstance(block, str):
                            parts.append(block)
                    text = "\n".join(parts)
                else:
                    continue

                if text.strip():
                    messages.append(f"[{role}]: {text.strip()}")
    except Exception as e:
        print(f"[oe_auto_ingest] error reading {latest}: {e}", file=sys.stderr)
        return "", ""

    return "\n\n".join(messages), session_id

def process_source(source_name: str, content: str, force: bool = False) -> dict:
    """process content from a source, ingest if threshold reached."""
    state = load_state()

    tokens = count_tokens(content)

    if source_name not in state["sources"]:
        state["sources"][source_name] = {
            "pending_tokens": 0,
            "total_ingested": 0,
            "last_offset": 0,
            "ingest_count": 0
        }

    src = state["sources"][source_name]

    # get only new content (beyond what we've already seen)
    last_offset = src["last_offset"]
    if last_offset > 0 and last_offset < len(content):
        new_content = content[last_offset:]
    elif last_offset >= len(content):
        new_content = ""
    else:
        new_content = content

    new_tokens = count_tokens(new_content)
    src["pending_tokens"] += new_tokens
    src["last_offset"] = len(content)

    result = {
        "source": source_name,
        "new_tokens": new_tokens,
        "pending_tokens": src["pending_tokens"],
        "threshold": TOKEN_THRESHOLD,
        "ingested": False
    }

    if src["pending_tokens"] >= TOKEN_THRESHOLD or (force and new_tokens > 0):
        if not oe_health():
            result["error"] = "OE server not reachable"
            save_state(state)
            return result

        # chunk the pending content and ingest
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        filename = f"chat_{source_name}_{timestamp}.txt"

        if oe_ingest(filename, new_content):
            src["total_ingested"] += src["pending_tokens"]
            state["total_ingested_tokens"] += src["pending_tokens"]
            src["pending_tokens"] = 0
            src["ingest_count"] += 1
            state["last_ingest"] = time.strftime("%Y-%m-%d %H:%M:%S")
            result["ingested"] = True
            result["filename"] = filename
            result["tokens_sent"] = new_tokens
        else:
            result["error"] = "ingest request failed"

    save_state(state)
    return result

def show_status():
    state = load_state()
    print("=== OE Auto-Ingest Status ===")
    print(f"OE server: {'online' if oe_health() else 'offline'}")
    print(f"Token threshold: {TOKEN_THRESHOLD:,}")
    print(f"Total ingested: {state.get('total_ingested_tokens', 0):,} tokens")
    print(f"Last ingest: {state.get('last_ingest', 'never')}")
    print()
    for name, src in state.get("sources", {}).items():
        print(f"  [{name}]")
        print(f"    pending: {src['pending_tokens']:,} tokens")
        print(f"    total ingested: {src['total_ingested']:,} tokens")
        print(f"    ingest count: {src['ingest_count']}")
    if not state.get("sources"):
        print("  (no sources tracked yet)")

def main():
    parser = argparse.ArgumentParser(description="auto-ingest chat content to Ocean Eterna")
    parser.add_argument("--file", help="file to ingest")
    parser.add_argument("--stdin", action="store_true", help="read from stdin")
    parser.add_argument("--source", default="generic", help="source name for tracking")
    parser.add_argument("--claude-code", action="store_true", help="ingest current Claude Code session")
    parser.add_argument("--force", action="store_true", help="force ingest regardless of threshold")
    parser.add_argument("--status", action="store_true", help="show ingest status")
    parser.add_argument("--threshold", type=int, help="override token threshold")
    args = parser.parse_args()

    if args.threshold:
        global TOKEN_THRESHOLD
        TOKEN_THRESHOLD = args.threshold

    if args.status:
        show_status()
        return

    if args.claude_code:
        content, session_id = extract_claude_code_content()
        if not content:
            # exit cleanly — no conversation to ingest is not an error
            sys.exit(0)
        source_name = f"claude_code_{session_id[:8]}" if session_id else "claude_code"
        result = process_source(source_name, content, args.force)
    elif args.stdin:
        content = sys.stdin.read()
        result = process_source(args.source, content, args.force)
    elif args.file:
        if not os.path.exists(args.file):
            print(f"[oe_auto_ingest] file not found: {args.file}", file=sys.stderr)
            sys.exit(1)
        with open(args.file) as f:
            content = f.read()
        source_name = args.source if args.source != "generic" else Path(args.file).stem
        result = process_source(source_name, content, args.force)
    else:
        parser.print_help()
        return

    if result.get("ingested"):
        print(f"[oe_auto_ingest] ingested {result.get('tokens_sent', 0):,} tokens from {result['source']} -> {result.get('filename', '?')}")
    elif result.get("error"):
        print(f"[oe_auto_ingest] {result['error']}", file=sys.stderr)
    else:
        pending = result.get("pending_tokens", 0)
        threshold = result.get("threshold", TOKEN_THRESHOLD)
        print(f"[oe_auto_ingest] {result['source']}: {pending:,}/{threshold:,} tokens pending ({result.get('new_tokens', 0):,} new)")

if __name__ == "__main__":
    main()
