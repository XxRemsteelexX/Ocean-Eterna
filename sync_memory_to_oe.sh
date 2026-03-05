#!/bin/bash
# sync_memory_to_oe.sh — sync Claude Code memories to Ocean Eterna
# runs as a Stop/SessionEnd hook so OE stays current with Claude's knowledge
OE_URL="http://localhost:9090"

# quick health check — skip if server is down
curl -sf "${OE_URL}/stats" > /dev/null 2>&1 || exit 0

# collect all CLAUDE.md files from active projects
for md in /home/yeblad/CLAUDE.md /home/yeblad/*/CLAUDE.md /home/yeblad/*/*/CLAUDE.md; do
    [ -f "$md" ] || continue
    project=$(basename "$(dirname "$md")")
    content=$(cat "$md")
    [ -z "$content" ] && continue

    curl -s -X POST "${OE_URL}/add-file" \
        -H "Content-Type: application/json" \
        -d "$(jq -n --arg fn "claude_memory_${project}.txt" --arg c "$content" '{filename: $fn, content: $c}')" \
        > /dev/null 2>&1
done

# also sync the auto-memory file
if [ -f /home/yeblad/.claude/projects/-home-yeblad/memory/MEMORY.md ]; then
    content=$(cat /home/yeblad/.claude/projects/-home-yeblad/memory/MEMORY.md)
    if [ -n "$content" ]; then
        curl -s -X POST "${OE_URL}/add-file" \
            -H "Content-Type: application/json" \
            -d "$(jq -n --arg fn "claude_auto_memory.txt" --arg c "$content" '{filename: $fn, content: $c}')" \
            > /dev/null 2>&1
    fi
fi
