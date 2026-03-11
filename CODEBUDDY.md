# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this
repository.

## Build Commands

```bash
# Initial setup (first time only)
./autogen.sh
./configure
make

# Rebuild after changes
make

# Clean build
make clean && make

# Configure with pcap support (optional, for packet capture)
./configure --disable-pcap  # to disable
./configure                  # auto-detects pcap
```

## Prerequisites

- `libevent` (required)
- `autotools` (autoconf, automake - for build system)
- `make`
- `libpcap` (optional - for packet capture via `-p` option)

## Project Overview

tunsocks is a user-level proxy that bridges VPN traffic (typically via tun
devices) with SOCKS, HTTP proxies, and port forwarding. It uses lwIP
(lightweight IP stack) to handle network protocols entirely in userspace.

## Architecture

### Core Components

1. **lwIP Stack** (`lwip/`) - Embedded TCP/IP stack submodule that handles all
   network protocol processing in userspace. Configured via
   `include/lwipopts.h`.

2. **lwip-libevent** (`lwip-libevent/`) - Integration layer between lwIP and
   libevent:
   - `libevent.c` - Integrates lwIP timeouts into the libevent event loop
   - `netif/` - Various network interface implementations (fdif, slirpif,
     udptapif, etc.)
   - `util/lwipevbuf.c` - Bridges lwIP TCP connections with libevent evbuffers

3. **lwip-nat** (`lwip-nat/`) - SNAT/NAT implementation:
   - Hooks into lwIP's IP input path via `LWIP_HOOK_IP4_INPUT`
   - Handles connection tracking and address translation for TCP, UDP, ICMP
   - Enables connection sharing through the VPN interface

4. **Application Layer** (`src/`):
   - `main.c` - Entry point, argument parsing, netif setup
   - `socks.c`, `socks4.c`, `socks5.c` - SOCKS proxy implementation (supports
     SOCKS 4, 4a, and 5)
   - `http/http.c`, `http/server.c` - HTTP proxy and PAC file server
   - `forward_local.c`, `forward_remote.c` - Local and remote port forwarding
   - `nat.c` - NAT interface setup for various backends (UDP, VDE, TUN/TAP)

### Network Interface Flow

```
VPN (stdin/stdout or fd) <---> fdif (netif) <---> lwIP stack <---> Proxy listeners
                                                                  |
                                                                  v
                                                          lwipevbuf <---> bufferevent (local socket)
```

- **fdif** - File descriptor interface, reads packets from VPN (stdin/fd),
  injects into lwIP
- **slirpif** - Uses host's IP stack for outbound connections (testing mode)
- **udptapif** - UDP-based NAT interface for QEMU/VirtualBox
- **tunif** - Native TUN/TAP device interface
- **vdeportif/vdeswitchif** - VDE switch connectivity

### Key Data Structures

- `struct lwipevbuf` - Bridges lwIP TCP PCB with libevent evbuffers, similar to
  bufferevent API
- `struct socks_data` - Per-connection state for SOCKS proxy sessions
- `struct nat_pcb` - NAT connection tracking entry

### lwIP Integration Notes

- Uses `NO_SYS=1` and `NO_SYS_NO_TIMERS=1` - no RTOS, libevent handles timing
- `LWIP_HOOK_IP4_INPUT` is set to `ip4_input_nat` for NAT processing
- Memory allocation uses libc malloc (`MEM_LIBC_MALLOC=1`, `MEMP_MEM_MALLOC=1`)

## Submodules

- `lwip/` - lwIP TCP/IP stack (git submodule)

After cloning, run:

```bash
git submodule init
git submodule update
```

## Testing

For testing network behavior with simulated conditions:

```bash
# Add 100ms delay to packets
./tunsocks -l 100 ...

# Drop 10% of packets randomly
./tunsocks -o 0.1 ...

# Use slirp interface (bypass VPN, use host network)
./tunsocks -S ...
```
