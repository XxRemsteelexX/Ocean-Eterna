#!/bin/bash
# Build 10 segments of 5B tokens each for a 50B corpus
# Usage: ./build_segments.sh /path/to/data /path/to/output

DATA_DIR="${1:?Usage: $0 DATA_DIR OUTPUT_DIR}"
OUT_DIR="${2:?Usage: $0 DATA_DIR OUTPUT_DIR}"
SEGMENT_SIZE=5000000000  # 5B tokens per segment
NUM_SEGMENTS=10

echo "Building $NUM_SEGMENTS segments of $(echo $SEGMENT_SIZE | numfmt --to=si) tokens each"
echo "Data: $DATA_DIR"
echo "Output: $OUT_DIR"

mkdir -p "$OUT_DIR"

for i in $(seq 0 $((NUM_SEGMENTS - 1))); do
    SEG_DIR=$(printf "%s/seg_%03d" "$OUT_DIR" "$i")
    SKIP=$((i * SEGMENT_SIZE))

    echo ""
    echo "=== Building segment $i → $SEG_DIR ==="
    echo "  skip-tokens: $SKIP, max-tokens: $SEGMENT_SIZE"

    ./bulk_build \
        --dirs "$DATA_DIR" \
        --out "$SEG_DIR" \
        --max-tokens "$SEGMENT_SIZE" \
        --skip-tokens "$SKIP" \
        --neighbor-top 0 \
        --threads "$(nproc)"

    echo "Segment $i complete: $(du -sh "$SEG_DIR" | cut -f1)"
done

# Generate meta.json
echo ""
echo "Generating meta.json..."
python3 -c "
import json, os
segments = []
for i in range($NUM_SEGMENTS):
    seg_dir = f'seg_{i:03d}'
    if os.path.exists(os.path.join('$OUT_DIR', seg_dir, 'stems.idx')):
        segments.append({'id': i, 'dir': seg_dir, 'tokens': $SEGMENT_SIZE})
meta = {
    'version': '9.1',
    'description': f'{len(segments)} segments of {$SEGMENT_SIZE} tokens',
    'segments': segments
}
with open(os.path.join('$OUT_DIR', 'meta.json'), 'w') as f:
    json.dump(meta, f, indent=2)
print(f'meta.json: {len(segments)} segments')
"

echo ""
echo "Done. Start server with:"
echo "  ./ocean_chat_server --manifest $OUT_DIR/seg_000/manifest.jsonl --storage $OUT_DIR/seg_000/storage.bin --port 9292"
