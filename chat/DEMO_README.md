# OceanEterna Demo Interface

## Quick Start

```bash
# 1. Start the server
cd /media/yeblad/Ocean\ Eterna/Ocean/main/src/chat
./ocean_chat_server

# 2. Open in browser
# Open: ocean_demo.html
```

## Features Shown in Demo

### Left Panel
- **Token Count** - 9.4M tokens indexed
- **Chunk Count** - 22,000+ chunks
- **Speed Bars** - Visual: Search (40ms) vs LLM (2000ms+)
- **Compression** - 147:1 ratio display
- **Corpus Overview** - DOC/CHAT/CODE/FIX/FEAT breakdown

### Center Panel
- Chat interface with source references
- Speed badges on each response
- Clickable chunk IDs

### Right Panel
- "Tell Me More" button (no re-search!)
- File upload
- System stats
- Controls

## Demo Script

1. **Show the stats**: "9 million tokens loaded in under 200ms"
2. **Ask a question**: "What is Moby Dick about?"
3. **Point out speed**: "Search took 40ms, LLM took 2 seconds"
4. **Show sources**: "See the chunk IDs - that's 147:1 compression"
5. **Tell Me More**: "Watch - search time is now 0ms, using cached sources"
6. **Reveal**: "This is running from a USB drive, CPU only, no GPU"

## Key Selling Points

- **Speed**: 40ms search on 9M tokens
- **Compression**: 147:1 context compression via chunk IDs
- **No GPU**: Runs on CPU only
- **Portable**: Runs from USB drive
- **Infinite Context**: Chunk IDs enable unlimited effective context

## Troubleshooting

**"Disconnected" status:**
- Make sure server is running on port 8888
- Check: `curl http://localhost:8888/stats`

**Corpus Overview shows 0:**
- Refresh the page after server starts
- Check: `curl http://localhost:8888/guide`

**Page scrolls instead of chat:**
- Hard refresh: Ctrl+Shift+R
