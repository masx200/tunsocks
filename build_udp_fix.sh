#!/bin/bash
# Build script for tunsocks with UDP ASSOCIATE fix

set -e

echo "=== Building tunsocks with UDP ASSOCIATE fix ==="
echo ""
echo "Changes made:"
echo "1. Added client address storage to socks_data structure"
echo "2. Removed connect() call from socks_udp_bind()"
echo "3. Changed recv() to recvfrom() in socks_udp_read()"
echo "4. Changed send() to sendto() in socks5_udp_recv()"
echo ""
echo "This fixes SOCKS5 UDP ASSOCIATE to support multiple destinations."
echo ""

# Check if autogen.sh exists
if [ ! -f "autogen.sh" ]; then
    echo "Error: autogen.sh not found. Please run this script from tunsocks directory."
    exit 1
fi

# Run autogen
echo "Running autogen.sh..."
./autogen.sh

# Configure
echo "Running configure..."
./configure

# Build
echo "Building tunsocks..."
make

echo ""
echo "=== Build complete! ==="
echo "The tunsocks binary should now support proper SOCKS5 UDP ASSOCIATE."
echo ""
echo "To test the fix:"
echo "1. Recompile and install tunsocks"
echo "2. Restart your OpenVPN service"
echo "3. Test UDP connectivity through the SOCKS5 proxy"
