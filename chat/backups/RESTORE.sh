#!/bin/bash
# Restore script for OceanEterna 100% accuracy version
# Date: January 31, 2026

echo "Restoring OceanEterna to 100% accuracy version (2026-01-31)..."

# Stop running server
pkill -f ocean_chat_server 2>/dev/null
sleep 1

# Restore source code
cp /home/yeblad/OE_1.24.26/chat/backups/ocean_chat_server_100pct_accuracy_2026-01-31.cpp \
   /home/yeblad/OE_1.24.26/chat/ocean_chat_server.cpp

# Restore binary
cp /home/yeblad/OE_1.24.26/chat/backups/ocean_chat_server_100pct_accuracy_2026-01-31 \
   /home/yeblad/OE_1.24.26/chat/ocean_chat_server

echo "Restore complete!"
echo ""
echo "To start server:"
echo "  cd /home/yeblad/OE_1.24.26/chat"
echo "  ./ocean_chat_server 8888"
