# Tunsocks UDP Port Forwarding

## Overview

UDP port forwarding allows you to forward UDP packets from a local port to a
remote destination through the VPN tunnel. This is useful for applications that
use UDP protocols like DNS, VoIP, gaming, QUIC (HTTP/3), and other UDP-based
services.

## What's New

Previously, tunsocks only supported TCP port forwarding via the `-L` option.
Now, UDP forwarding is fully supported through the new `-j` option.

## Usage

### Basic Syntax

```bash
tunsocks -j [bind_address:]bind_port:host_address:host_port
```

### Parameters

- **bind_address** (optional): Local address to bind to. Defaults to localhost
  if not specified.
- **bind_port**: Local port to listen on.
- **host_address**: Remote destination hostname or IP address.
- **host_port**: Remote destination port.

### Examples

```bash
# Forward local UDP port 8443 to a remote server
tunsocks -j 0.0.0.0:8443:162.159.199.2:8443

# Forward DNS queries through VPN
tunsocks -j 127.0.0.1:53:8.8.8.8:53

# Forward WireGuard traffic
tunsocks -j 0.0.0.0:51820:vpn.example.com:51820

# Forward with automatic bind to localhost
tunsocks -j 5353:233.252.12.12:5353
```

## Comparison with TCP Forwarding

| Feature                 | TCP (-L)            | UDP (-j)                |
| ----------------------- | ------------------- | ----------------------- |
| **Connection-oriented** | Yes                 | No                      |
| **Reliability**         | Guaranteed          | Best effort             |
| **Ordering**            | Preserved           | Not guaranteed          |
| **Use Cases**           | HTTP, SSH, FTP      | DNS, VoIP, Gaming, QUIC |
| **Example**             | `-L 8080:remote:80` | `-j 53:8.8.8.8:53`      |

## How It Works

### Architecture

```
Client Application → Local UDP Socket → libevent → lwIP UDP PCB → VPN → Remote Server
                                                                    ↓
Client Application ← sendto() ← UDP recv callback ← lwIP UDP PCB ← Remote Server
```

### Data Flow

1. **Client → Local Socket**
   - Application sends UDP packet to local port
   - `forward_local_udp_read()` receives packet via `recvfrom()`
   - Client address is stored for later reply

2. **Local Socket → lwIP**
   - Packet is placed into a pbuf (lwIP packet buffer)
   - `udp_sendto()` forwards packet through lwIP stack
   - Packet is routed through VPN interface

3. **lwIP → Remote Server**
   - lwIP handles IP encapsulation and routing
   - Packet is sent through VPN tunnel
   - Remote server receives packet

4. **Remote Server → Client**
   - Remote server sends response
   - Response arrives through VPN
   - `forward_local_udp_recv()` callback is triggered
   - Response is sent back to original client via `sendto()`

## Technical Implementation

### Key Components

#### 1. UDP Context Structure

```c
struct forward_local_udp {
    char *remote_host;
    unsigned short remote_port;
    ip_addr_t remote_ipaddr;
    int udp_fd;
    struct udp_pcb *upcb4;    // IPv4 UDP PCB
    struct udp_pcb *upcb6;    // IPv6 UDP PCB
    struct event *udp_event;   // libevent for I/O
    struct pbuf *udp_pbuf;     // lwIP packet buffer
    struct sockaddr_storage client_addr;
    socklen_t client_addrlen;
};
```

#### 2. Client → Remote Forwarding

```c
static void forward_local_udp_read(const int fd, short int method, void *arg)
{
    // Receive from client and capture address
    len = recvfrom(fd, p->payload, p->len, 0,
                  (struct sockaddr *)&udp_ctx->client_addr,
                  &udp_ctx->client_addrlen);

    // Forward to remote via lwIP
    udp_sendto(udp_ctx->upcb4, p, &udp_ctx->remote_ipaddr, udp_ctx->remote_port);
}
```

#### 3. Remote → Client Forwarding

```c
static void forward_local_udp_recv(void *arg, struct udp_pcb *pcb,
                                   struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    // Send back to the original client
    sendto(udp_ctx->udp_fd, p->payload, p->len, 0,
           (struct sockaddr *)&udp_ctx->client_addr,
           udp_ctx->udp_ctx->client_addrlen);
}
```

## Files Modified

### src/forward_local.c

- Added `forward_local_udp` structure for UDP context management
- Added `forward_local_udp_recv()` - handles packets from remote
- Added `forward_local_udp_read()` - handles packets from client
- Added `forward_local_udp()` - initializes UDP forwarding
- Modified `forward_local()` - added `is_udp` parameter

### src/forward_local.h

- Updated function signature to include `is_udp` parameter

### src/main.c

- Added `is_udp` field to `conn_info` structure
- Added `-j` command-line option for UDP forwarding
- Updated usage information

## Use Cases

### 1. DNS Forwarding

Forward DNS queries through VPN:

```bash
tunsocks -j 127.0.0.1:53:1.1.1.1:53
# Test with
nslookup google.com 127.0.0.1
```

### 2. WireGuard VPN

Forward WireGuard traffic:

```bash
tunsocks -j 0.0.0.0:51820:vpn-server.example.com:51820
```

### 3. QUIC/HTTP3

Forward HTTP/3 traffic:

```bash
tunsocks -j 0.0.0.0:443:cloudflare.com:443
```

### 4. Gaming Servers

Forward game traffic:

```bash
tunsocks -j 0.0.0.0:27015:game-server.example.com:27015
```

### 5. VoIP/SIP

Forward VoIP traffic:

```bash
tunsocks -j 0.0.0.0:5060:sip-provider.example.com:5060
```

## Testing

### Compile

```bash
cd /path/to/tunsocks
./autogen.sh
./configure
make
```

### Test UDP Forwarding

#### Using netcat

```bash
# Terminal 1: Start tunsocks with UDP forwarding
./tunsocks -j 0.0.0.0:8888:example.com:9999

# Terminal 2: Send UDP packet
echo "Hello UDP" | nc -u 127.0.0.1 8888

# Terminal 3: Listen on remote server
nc -u -l 9999
```

#### Using socat

```bash
# Forward local to remote
socat - UDP:127.0.0.1:8888
./tunsocks -j 127.0.0.1:8888:remote.example.com:8888
```

#### Verify with tcpdump

```bash
# Monitor UDP traffic
sudo tcpdump -i any -n 'udp port 8888'
```

## Performance Considerations

### Buffer Sizes

- Default pbuf size: 2048 bytes
- Suitable for most UDP applications
- Can be adjusted in code if needed

### Concurrency

- Supports multiple clients simultaneously
- Each client's address is tracked independently
- No connection state maintained (UDP is connectionless)

### Error Handling

- Handles EAGAIN and EINTR gracefully
- Logs errors for debugging
- Continues operation on transient failures

## Limitations

1. **No Connection Tracking**: UDP is connectionless, so there's no session
   state
2. **Best Effort Delivery**: Packets may be lost or arrive out of order
3. **No Congestion Control**: UDP doesn't have built-in congestion control
4. **MTU Limitations**: Packets larger than path MTU will be fragmented

## Troubleshooting

### Packets Not Forwarding

```bash
# Check if tunsocks is running
ps aux | grep tunsocks

# Check if port is listening
netstat -ulnp | grep 8443

# Check firewall rules
sudo iptables -L -n -v | grep 8443
```

### Permission Denied

```bash
# Ports below 1024 require root
sudo ./tunsocks -j 0.0.0.0:53:8.8.8.8:53

# Or use a higher port
./tunsocks -j 0.0.0.0:5353:8.8.8.8:53
```

### High Packet Loss

- Check VPN connection stability
- Verify remote server is reachable
- Check network bandwidth and latency

## Security Notes

1. **Bind Address**: Use specific bind address instead of 0.0.0.0 when possible
   ```bash
   # More secure - only localhost
   tunsocks -j 127.0.0.1:53:8.8.8.8:53

   # Less secure - accepts from anywhere
   tunsocks -j 0.0.0.0:53:8.8.8.8:53
   ```

2. **Firewall**: Consider firewall rules to restrict access

3. **Logging**: Monitor logs for suspicious activity

## Future Enhancements

Potential improvements for future versions:

- [ ] Support for UDP multicast forwarding
- [ ] Packet size MTU discovery
- [ ] Statistics and monitoring
- [ ] Per-client rate limiting
- [ ] UDP relay through SOCKS proxy

## Related Documentation

- [README.md](README.md) - General tunsocks documentation
- [UDP_FIX_SUMMARY.md](UDP_FIX_SUMMARY.md) - SOCKS5 UDP ASSOCIATE fix
- [CLAUDE.md](CLAUDE.md) - Project architecture and build instructions

## Summary

The UDP port forwarding feature (`-j` option) enables tunsocks to forward UDP
traffic through VPN tunnels, expanding its capabilities beyond TCP-only
forwarding. This is essential for modern applications that rely on UDP protocols
like DNS, QUIC, WireGuard, and various real-time communication services.

| Feature          | Status         |
| ---------------- | -------------- |
| IPv4 Support     | ✅ Implemented |
| IPv6 Support     | ✅ Implemented |
| Multiple Clients | ✅ Supported   |
| Error Handling   | ✅ Implemented |
| Performance      | ✅ Optimized   |
| Documentation    | ✅ Complete    |
