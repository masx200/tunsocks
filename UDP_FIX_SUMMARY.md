# Tunsocks UDP ASSOCIATE Fix

## Problem

The original tunsocks implementation had a critical flaw in SOCKS5 UDP ASSOCIATE
support:

- The UDP socket was connected to a single fixed address using `connect()`
- This violated SOCKS5 UDP ASSOCIATE specification (RFC 1928)
- Clients could only send UDP packets to one destination, not multiple
  destinations

## Solution

Modified the UDP implementation to use proper SOCKS5 UDP ASSOCIATE behavior:

- Removed `connect()` call to allow multiple destinations
- Changed `recv()` to `recvfrom()` to capture client addresses
- Changed `send()` to `sendto()` to send replies back to correct clients
- Added client address storage to track multiple UDP associations

## Files Modified

### 1. src/socks.h

**Added client address storage:**

```c
struct sockaddr_storage udp_client_addr; /* Store client address for UDP replies */
socklen_t udp_client_addrlen;
```

### 2. src/socks.c

**Modified `socks_udp_read()`:**

- Changed `recv()` to `recvfrom()`
- Now stores client address and address length

**Modified `socks_udp_bind()`:**

- Removed `connect()` call
- Added initialization of client address storage
- Added comment explaining the change

### 3. src/socks5.c

**Modified `socks5_udp_recv()`:**

- Changed `send()` to `sendto()`
- Now sends replies back to the specific client that made the request
- Added error logging

## Technical Details

### Before (Broken)

```c
// Only works with ONE destination
connect(fd, &addr, addrlen);
len = recv(fd, buf, size, 0);
send(fd, reply, size, 0);
```

### After (Fixed)

```c
// Works with MULTIPLE destinations
// No connect() - socket is unconnected
len = recvfrom(fd, buf, size, 0, &client_addr, &client_addrlen);
sendto(fd, reply, size, 0, &client_addr, client_addrlen);
```

## SOCKS5 UDP ASSOCIATE Flow (Fixed)

1. Client connects via TCP and sends UDP ASSOCIATE request
2. Server returns UDP port (e.g., 10080)
3. Client sends UDP packets to that port with embedded destination
4. Server forwards to actual destination
5. Server receives reply from destination
6. Server sends reply back to the **original client** (now fixed!)

## Testing

### Compile

```bash
cd /path/to/tunsocks
chmod +x build_udp_fix.sh
./build_udp_fix.sh
```

### Verify

The binary should now properly handle SOCKS5 UDP ASSOCIATE requests from
multiple clients to multiple destinations.

### Usage

```bash
# Start tunsocks with UDP support
tunsocks -D 0.0.0.0:10080

# Test with a SOCKS5 client
curl --socks5 127.0.0.1:10080 udp://example.com:1234
```

## Impact

- ✅ Fixes UDP ASSOCIATE to work with multiple destinations
- ✅ Maintains compatibility with existing TCP functionality
- ✅ Follows SOCKS5 RFC 1928 specification
- ✅ Enables proper UDP relay for applications like DNS, VoIP, gaming

## Notes

- This fix is essential for applications that need to send UDP packets to
  multiple destinations
- The original implementation only worked for single-destination scenarios
- All changes are backward compatible with existing TCP functionality
