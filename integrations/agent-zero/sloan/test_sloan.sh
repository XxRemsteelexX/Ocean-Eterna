#!/bin/bash
# smoke test for sloan agent
# uses plain docker commands (no compose required)
# usage: bash test_sloan.sh
set -euo pipefail

SLOAN_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SLOAN_DIR/../../.." && pwd)"
IMAGE_NAME="sloan"
CONTAINER_NAME="sloan"

cd "$SLOAN_DIR"

echo "=== sloan agent smoke test ==="

# 1. seed settings.json into the persistent volume if not present
if [ ! -f sloan-data/settings.json ]; then
    echo "[1/5] seeding settings.json with OE MCP config..."
    mkdir -p sloan-data
    cp settings.json sloan-data/settings.json
else
    echo "[1/5] settings.json already exists in sloan-data/"
fi

# 2. build
echo "[2/5] building sloan image..."
docker build \
    -f "$SLOAN_DIR/Dockerfile" \
    -t "$IMAGE_NAME" \
    "$REPO_ROOT"

# 3. stop old container if running
if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "       stopping existing sloan container..."
    docker stop "$CONTAINER_NAME" 2>/dev/null || true
    docker rm "$CONTAINER_NAME" 2>/dev/null || true
fi

# 4. start
echo "[3/5] starting sloan container..."
docker run -d \
    --name "$CONTAINER_NAME" \
    -p 50001:80 \
    -v "$SLOAN_DIR/sloan-data:/a0/usr" \
    --add-host=host.docker.internal:host-gateway \
    --env-file "$SLOAN_DIR/.env" \
    --restart unless-stopped \
    "$IMAGE_NAME"

# 5. wait for health
echo "[4/5] waiting for agent zero to start..."
MAX_WAIT=90
ELAPSED=0
until curl -sf http://localhost:50001 > /dev/null 2>&1; do
    sleep 3
    ELAPSED=$((ELAPSED + 3))
    if [ $ELAPSED -ge $MAX_WAIT ]; then
        echo "FAIL: sloan did not become healthy within ${MAX_WAIT}s"
        echo "logs:"
        docker logs --tail=30 "$CONTAINER_NAME"
        exit 1
    fi
done
echo "       agent zero UI is up at http://localhost:50001"

# 6. verify
echo "[5/5] checking container status..."
docker ps --filter "name=$CONTAINER_NAME" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
echo ""
echo "=== sloan is running ==="
echo "  UI:   http://localhost:50001"
echo "  logs: docker logs -f $CONTAINER_NAME"
echo "  stop: docker stop $CONTAINER_NAME && docker rm $CONTAINER_NAME"
echo ""
echo "next steps:"
echo "  1. add an API key to $SLOAN_DIR/.env (anthropic, openai, etc.)"
echo "  2. open http://localhost:50001 in your browser"
echo "  3. verify 'ocean-eterna' appears in settings > MCP servers"
echo "  4. ask sloan to search your knowledge base"
