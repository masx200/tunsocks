# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

**First-time setup:**
```bash
# Clone and initialize git submodules (lwip is a submodule)
git submodule init
git submodule update

# Initialize build system
./autogen.sh

# Configure (with optional pcap support)
./configure
# ./configure --disable-pcap  # without pcap

# Build
make
```

**Rebuilding:**
```bash
# Clean build artifacts
make clean

# Rebuild
make
```

**Dependencies:**
- `libevent` - Required (event loop library)
- `libpcap` - Optional (for packet capture support via `-p` option)
- `autotools` - Required for building from git (autoconf, automake)

## Project Architecture

tunsocks is a user-level proxy server that implements SOCKS4/5, HTTP proxy, and port forwarding. It's designed to work with VPN clients (like OpenConnect) by accepting network traffic via stdin/stdout or a file descriptor instead of a tun device.

### Core Design

**Data Flow:**
```
VPN Client → stdin/fd → tunsocks (lwIP stack + NAT) → Network
Network → tunsocks → stdout/fd → VPN Client
```

**Key Abstraction Layers:**
1. **Network Interfaces** (`lwip-libevent/netif/`): Different ways to receive/send packets
   - `fdif.c` - File descriptor (stdin/stdout or VPNFD env var)
   - `slirpif.c` - Use host IP stack via libslirp
   - `tunif.c` - TUN device (point-to-point IP layer)
   - `udptapif.c` - UDP tunnel with TAP-style Ethernet frames
   - `vdeportif.c`, `vdeswitchif.c` - VDE switch integration

2. **Protocol Stack** (`lwip/`): Lightweight TCP/IP stack
   - Full IP stack implementation in userspace
   - Protocol: IPv4, IPv6, TCP, UDP, ICMP, DNS
   - Configured via `include/lwipopts.h`

3. **NAT Layer** (`lwip-nat/`): Source NAT for connection sharing
   - Tracks connections via `nat_pcb` structures
   - Supports TCP, UDP, ICMP4
   - Uses prerouting hook (via `ip4_input_nat.c`) to modify packets before routing
   - Each protocol implements 4 functions: find rule, modify packet, find ICMP rule, modify ICMP

4. **Proxy Protocols** (`src/`):
   - `socks.c` - Common SOCKS functionality
   - `socks4.c`, `socks5.c` - Protocol-specific handlers
   - `forward_local.c` - Local port forwarding (-L)
   - `forward_remote.c` - Remote port forwarding (-R)
   - `http/` - HTTP/HTTPS proxy implementation
   - `dhcp_server.c` - DHCP server for NAT interfaces

5. **Event Loop** (`lwip-libevent/libevent.c`): Bridges libevent and lwIP

6. **DHCP Server** (`lwip-udhcpd/`): Provides DHCP for NAT clients (TAP/UDP/VDE interfaces)

### SOCKS UDP ASSOCIATE Implementation

**Important Context**: The SOCKS5 UDP implementation recently received a critical fix (March 2026) to support multiple destinations properly.

**How it works:**
1. Client sends UDP ASSOCIATE request over TCP connection
2. Server allocates UDP socket and returns port to client
3. Client sends UDP packets to that port with embedded destination address (SOCKS5 UDP header format)
4. Server strips SOCKS5 header, forwards to actual destination using lwIP stack
5. Response packets from destination receive SOCKS5 header and are sent back to client

**Critical Implementation Details:**
- UDP socket must **NOT call `connect()`** - it must remain unconnected to support multiple destinations
- Use `recvfrom()` to capture client address for each packet
- Use `sendto()` to send responses back to specific clients
- Store client address in `socks_data::udp_client_addr` and `udp_client_addrlen`

**Key Files:**
- `src/socks.h` - Defines `socks_data` structure with UDP client address tracking
- `src/socks.c::socks_udp_read()` - Receives UDP from SOCKS client via `recvfrom()`
- `src/socks5.c::socks5_udp_recv()` - Adds SOCKS5 UDP header and sends via `sendto()`
- `src/socks5.c::socks5_udp_send()` - Strips SOCKS5 header and forwards to destination via lwIP

### Network Interface Naming Convention

When using `-p pcap_file[:netif]` for packet capture, netif names are:
- `fd` - File descriptor (VPN stdin/stdout)
- `sl` - Slirp interface (host IP stack)
- `ut` - UDP TAP without length header (-u option)
- `uh` - UDP TAP with 2-byte length header (-U option)
- `vp` - VDE port (connects to VDE switch, -v option)
- `vs` - VDE switch (emulates switch, -V option)
- `tu` - TUN device (point-to-point IP layer, -t option)
- `ta` - TAP device (Ethernet layer with DHCP, -T option)

### NAT Network Ranges

Each NAT interface type uses a different private network:
- UDP TAP (-u/-U): `10.0.4.0/24`
- VDE port (-v): `10.0.5.0/24`
- VDE switch (-V): `10.0.6.0/24`
- TAP device (-T): `10.0.7.0/24`
- TUN device (-t): `10.0.8.1` (point-to-point, no DHCP)

### Configuration Environment Variables

The proxy accepts network configuration via environment variables:
- `INTERNAL_IP4_ADDRESS` - Client IP address
- `INTERNAL_IP4_MTU` - MTU size
- `INTERNAL_IP4_DNS` - DNS servers (comma-separated)
- `CISCO_DEF_DOMAIN` - Domain search list
- `VPNFD` - File descriptor to use instead of stdin/stdout

### lwIP Integration Notes

**Memory Management:**
- Uses `pbuf` structures for packet buffers
- `PBUF_RAW` for raw packets, `PBUF_TRANSPORT` for UDP/TCP
- Header space allocation: `PBUF_LINK_ENCAPSULATION_HLEN + PBUF_LINK_HLEN + PBUF_IP_HLEN + PBUF_TRANSPORT_HLEN`

**IPv4/IPv6 Dual Stack:**
- Code uses `#if LWIP_IPV4` and `#if LWIP_IPV6` conditionals
- Both stacks run simultaneously when enabled
- ATYP (address type) handling in SOCKS5 supports IPv4 (0x01), IPv6 (0x04), and domain names (0x03)

**Protocol Handlers:**
- UDP: `udp_recv()` callback set via `udp_recv(pcb, callback, user_data)`
- TCP: More complex, uses `tcp_arg()`, `tcp_accept()`, `tcp_err()` etc.

## Common Patterns

**Adding a New SOCKS Command:**
1. Define command constant in appropriate `socks*.c`
2. Add handler in request parsing (e.g., `socks5_read_hdr()`)
3. Implement protocol-specific logic using `socks_data` callbacks
4. Use `socks_request()` for stateful request reading

**Network Interface Implementation:**
1. Create `netif` structure in lwIP
2. Implement `netif->input` function to receive packets
3. Use `lwip_sendto()` or `tcp_write()` to transmit
4. Register with libevent for I/O events

**NAT Protocol Extension:**
See `lwip-nat/` README. Each protocol needs:
- Rule generation/matching
- Packet modification (forward and reverse directions)
- ICMP error packet handling
- Connection tracking structure

## Testing

**Manual Testing:**
```bash
# Start SOCKS5 proxy
./tunsocks -D 0.0.0.0:10080

# Test with curl
curl --socks5 127.0.0.1:10080 https://example.com

# Start HTTP proxy
./tunsocks -H 0.0.0.0:8080

# Port forwarding
./tunsocks -L 8080:remote.example.com:80
```

**Debug Output:**
- Set `LWIP_DEBUG` to 1 in `include/lwipopts.h` to enable debug messages
- Individual debug flags control specific subsystems (e.g., `SOCKS_DEBUG`, `UDP_DEBUG`)

## Project Structure Notes

- **Submodules**: `lwip` and other directories are git submodules
- **Build artifacts**: Generated by autotools, ignored by git
- **Testing**: No automated test suite; manual testing required
- **Documentation**: See `README.md` for usage examples and `UDP_FIX_SUMMARY.md` for recent UDP fixes

### lwIP Configuration (`include/lwipopts.h`)

Key settings that differ from lwIP defaults:
- `NO_SYS=1`, `NO_SYS_NO_TIMERS=1` - No OS, libevent handles all timers
- `LWIP_NAT=1` - NAT support enabled
- `IP_FORWARD=1` - IP forwarding enabled for NAT
- `MEMP_NUM_UDP_PCB=1024`, `MEMP_NUM_TCP_PCB=1024` - High connection limits
- `MEM_SIZE=16*1024*1024` - 16MB memory pool for packets
- `LWIP_HOOK_IP4_INPUT=ip4_input_nat` - NAT prerouting hook

Debug flags (set to `LWIP_DBG_ON` to enable):
- `LWIP_DEBUG`, `NETIF_DEBUG`, `NAT_DEBUG`, `SOCKS_DEBUG`
- `ETHARP_DEBUG`, `SLIRPIF_DEBUG`, `TAPNAT_DEBUG`
