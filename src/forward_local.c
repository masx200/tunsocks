#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/ip6_addr.h>

#include "forward_local.h"
#include "socks.h"
#include "util/lwipevbuf.h"
#include "util/lwipevbuf_bev_join.h"
#include "container_of.h"

struct forward_remote {
	char *host;
	unsigned short port;
	int keep_alive;
};

/* UDP forward context */
struct forward_local_udp {
	char *remote_host;
	unsigned short remote_port;
	ip_addr_t remote_ipaddr;
	int host_resolved;

	int udp_fd;
	struct udp_pcb *upcb4;
#if LWIP_IPV6
	struct udp_pcb *upcb6;
#endif
	struct event *udp_event;
	struct pbuf *udp_pbuf;
	int udp_pbuf_len;

	struct sockaddr_storage client_addr;
	socklen_t client_addrlen;
};

static void forward_local_accept(struct evconnlistener *evl,
	evutil_socket_t new_fd, struct sockaddr *addr, int socklen, void *ctx)
{
	struct event_base *base = evconnlistener_get_base(evl);
	struct forward_remote *remote = ctx;
	struct lwipevbuf *lwipevbuf;
	struct bufferevent *bev;

	lwipevbuf = lwipevbuf_new(NULL);

	if (remote->keep_alive) {
		lwipevbuf->pcb->so_options |= SOF_KEEPALIVE;
		lwipevbuf->pcb->keep_intvl = remote->keep_alive;
		lwipevbuf->pcb->keep_idle = remote->keep_alive;
	}

	if (lwipevbuf_connect_hostname(lwipevbuf, AF_UNSPEC, remote->host, remote->port) < 0) {
		lwipevbuf_free(lwipevbuf);
		return;
	}

	bev = bufferevent_socket_new(base, new_fd, BEV_OPT_CLOSE_ON_FREE);
	lwipevbuf_bev_join(bev, lwipevbuf, 256*1024, NULL, NULL, NULL, NULL, NULL, NULL);
}

/* UDP forward: receive from remote and send back to client */
static void forward_local_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
				const ip_addr_t *addr, u16_t port)
{
	struct forward_local_udp *udp_ctx = arg;
	int len;

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: received %d bytes from remote\n", __func__, p->len));

	/* Send the packet back to the client using sendto() */
	len = sendto(udp_ctx->udp_fd, p->payload, p->len, 0,
		    (struct sockaddr *)&udp_ctx->client_addr,
		    udp_ctx->client_addrlen);

	if (len < 0) {
		LWIP_DEBUGF(SOCKS_DEBUG, ("%s: sendto failed: %d\n", __func__, errno));
	}

	pbuf_free(p);
}

/* UDP forward: receive from client and forward to remote */
static void forward_local_udp_read(const int fd, short int method, void *arg)
{
	struct forward_local_udp *udp_ctx = arg;
	struct pbuf *p = udp_ctx->udp_pbuf;
	unsigned int offset;
	int len;
	err_t err;

	offset = PBUF_LINK_ENCAPSULATION_HLEN + PBUF_LINK_HLEN +
		 PBUF_IP_HLEN + PBUF_TRANSPORT_HLEN;

	/* Reset the pbuf and allocate network header space */
	p->len = p->tot_len = udp_ctx->udp_pbuf_len;
	p->payload = LWIP_MEM_ALIGN((void *)((u8_t *)p +
			LWIP_MEM_ALIGN_SIZE(sizeof(struct pbuf)) + offset));

	/* Receive data from client and capture client address */
	udp_ctx->client_addrlen = sizeof(udp_ctx->client_addr);
	len = recvfrom(fd, p->payload, p->len, 0,
		      (struct sockaddr *)&udp_ctx->client_addr,
		      &udp_ctx->client_addrlen);

	if (len < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		LWIP_DEBUGF(SOCKS_DEBUG, ("%s: UDP read error: %d\n", __func__, errno));
		return;
	}

	p->len = p->tot_len = len;
	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: received %d bytes from client\n", __func__, len));

	/* Forward to remote target via lwIP */
#if LWIP_IPV4
	if (IP_IS_V4(&udp_ctx->remote_ipaddr))
		err = udp_sendto(udp_ctx->upcb4, p, &udp_ctx->remote_ipaddr, udp_ctx->remote_port);
#endif
#if LWIP_IPV6
	if (IP_IS_V6(&udp_ctx->remote_ipaddr))
		err = udp_sendto(udp_ctx->upcb6, p, &udp_ctx->remote_ipaddr, udp_ctx->remote_port);
#endif

	if (err < 0)
		LWIP_DEBUGF(SOCKS_DEBUG, ("%s: udp_sendto failed: %d\n", __func__, err));
}

/* Setup UDP forwarding */
static int forward_local_udp(struct event_base *base,
	const char *local_host, const char *local_port,
	const char *remote_host, const char *remote_port)
{
	struct forward_local_udp *udp_ctx;
	struct addrinfo hints;
	struct addrinfo *result;
	struct event *event;
	struct udp_pcb *pcb;
	int fd;
	int ret;
	u_int16_t port;
	char *endptr;

	/* Parse remote port */
	port = strtoul(remote_port, &endptr, 0);
	if (endptr[0]) {
		struct servent *s;
		s = getservbyname(remote_port, "udp");
		if (s) {
			port = ntohs(s->s_port);
		} else {
			s = getservbyname(remote_port, "tcp");
			port = ntohs(s->s_port);
		}
		endservent();
		if (!s)
			return -1;
	}

	/* Allocate UDP context */
	udp_ctx = calloc(1, sizeof(*udp_ctx));
	if (!udp_ctx)
		return -1;

	udp_ctx->remote_host = strdup(remote_host);
	udp_ctx->remote_port = port;
	udp_ctx->host_resolved = 0;

	/* Resolve remote host to IP address */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	ret = getaddrinfo(remote_host, NULL, &hints, &result);
	if (ret < 0) {
		fprintf(stderr, "%s: getaddrinfo(%s): %s\n", __func__, remote_host, gai_strerror(ret));
		free(udp_ctx->remote_host);
		free(udp_ctx);
		return -1;
	}

	/* Store remote IP address */
	if (result->ai_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)result->ai_addr;
		ip_addr_t ipaddr;
		ip4_addr_t ip4;
		memcpy(&ip4, &sin->sin_addr, sizeof(ip4));
		ip_addr_copy_from_ip4(ipaddr, ip4);
		udp_ctx->remote_ipaddr = ipaddr;
#if LWIP_IPV6
	} else if (result->ai_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)result->ai_addr;
		ip_addr_t ipaddr;
		ip6_addr_t ip6;
		memcpy(&ip6, &sin6->sin6_addr, sizeof(ip6));
		ip_addr_copy_from_ip6(ipaddr, &ip6);
		udp_ctx->remote_ipaddr = ipaddr;
#endif
	} else {
		freeaddrinfo(result);
		free(udp_ctx->remote_host);
		free(udp_ctx);
		return -1;
	}
	freeaddrinfo(result);

	/* Create and bind UDP socket */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	ret = getaddrinfo(local_host, local_port, &hints, &result);
	if (ret < 0) {
		fprintf(stderr, "%s: getaddrinfo(%s:%s): %s\n", __func__, local_host, local_port, gai_strerror(ret));
		free(udp_ctx->remote_host);
		free(udp_ctx);
		return ret;
	}

	fd = socket(result->ai_family, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		perror("socket");
		freeaddrinfo(result);
		free(udp_ctx->remote_host);
		free(udp_ctx);
		return -1;
	}

	/* Set non-blocking */
	if (evutil_make_socket_nonblocking(fd) < 0) {
		perror("evutil_make_socket_nonblocking");
		close(fd);
		freeaddrinfo(result);
		free(udp_ctx->remote_host);
		free(udp_ctx);
		return -1;
	}

	if (bind(fd, result->ai_addr, result->ai_addrlen) < 0) {
		perror("bind");
		close(fd);
		freeaddrinfo(result);
		free(udp_ctx->remote_host);
		free(udp_ctx);
		return -1;
	}
	freeaddrinfo(result);

	/* Initialize client address storage */
	memset(&udp_ctx->client_addr, 0, sizeof(udp_ctx->client_addr));
	udp_ctx->client_addrlen = sizeof(udp_ctx->client_addr);

	/* Create lwIP UDP PCB */
#if LWIP_IPV4
	if (IP_IS_V4(&udp_ctx->remote_ipaddr)) {
		pcb = udp_new();
		if (!pcb) {
			fprintf(stderr, "%s: udp_new (IPv4) failed\n", __func__);
			close(fd);
			free(udp_ctx->remote_host);
			free(udp_ctx);
			return -1;
		}
		udp_bind(pcb, IP_ADDR_ANY, 0);
		udp_recv(pcb, forward_local_udp_recv, udp_ctx);
		udp_ctx->upcb4 = pcb;
	}
#endif
#if LWIP_IPV6
	if (IP_IS_V6(&udp_ctx->remote_ipaddr)) {
		pcb = udp_new();
		if (!pcb) {
			fprintf(stderr, "%s: udp_new (IPv6) failed\n", __func__);
			close(fd);
#if LWIP_IPV4
			if (udp_ctx->upcb4)
				udp_remove(udp_ctx->upcb4);
#endif
			free(udp_ctx->remote_host);
			free(udp_ctx);
			return -1;
		}
		udp_bind(pcb, IP6_ADDR_ANY, 0);
		udp_recv(pcb, forward_local_udp_recv, udp_ctx);
		udp_ctx->upcb6 = pcb;
	}
#endif

	/* Allocate pbuf for receiving */
	udp_ctx->udp_pbuf = pbuf_alloc(PBUF_RAW, 2048, PBUF_RAM);
	if (!udp_ctx->udp_pbuf) {
		fprintf(stderr, "%s: pbuf_alloc failed\n", __func__);
		close(fd);
#if LWIP_IPV4
		if (udp_ctx->upcb4)
			udp_remove(udp_ctx->upcb4);
#endif
#if LWIP_IPV6
		if (udp_ctx->upcb6)
			udp_remove(udp_ctx->upcb6);
#endif
		free(udp_ctx->remote_host);
		free(udp_ctx);
		return -1;
	}
	udp_ctx->udp_pbuf_len = 2048;
	udp_ctx->udp_fd = fd;

	/* Setup libevent for reading from UDP socket */
	event = event_new(base, fd, EV_READ|EV_PERSIST, forward_local_udp_read, udp_ctx);
	if (!event) {
		fprintf(stderr, "%s: event_new failed\n", __func__);
		pbuf_free(udp_ctx->udp_pbuf);
		close(fd);
#if LWIP_IPV4
		if (udp_ctx->upcb4)
			udp_remove(udp_ctx->upcb4);
#endif
#if LWIP_IPV6
		if (udp_ctx->upcb6)
			udp_remove(udp_ctx->upcb6);
#endif
		free(udp_ctx->remote_host);
		free(udp_ctx);
		return -1;
	}
	event_add(event, NULL);
	udp_ctx->udp_event = event;

	return 0;
}

#ifndef LEV_OPT_DEFERRED_ACCEPT
#define LEV_OPT_DEFERRED_ACCEPT 0
#endif

int forward_local(struct event_base *base,
	const char *local_host, const char *local_port,
	const char *remote_host, const char *remote_port, int keep_alive, int is_udp)
{
	if (is_udp) {
		/* UDP forwarding */
		return forward_local_udp(base, local_host, local_port,
					 remote_host, remote_port);
	}

	/* TCP forwarding */
	struct evconnlistener *evl;
	struct addrinfo hints;
	struct addrinfo *result;
	struct forward_remote *remote_ctx;
	u_int16_t port;
	char *endptr;
	int ret;

	port = strtoul(remote_port, &endptr, 0);
	if (endptr[0]) {
		struct servent *s;
		s = getservbyname(remote_port, "tcp");
		port = ntohs(s->s_port);
		endservent();
		if (!s)
			return -1;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	ret = getaddrinfo(local_host, local_port, &hints, &result);
	if (ret < 0) {
		fprintf(stderr, "%s: %s\n", __func__, gai_strerror(ret));
		return ret;
	}

	remote_ctx = calloc(1, sizeof(*remote_ctx));
	remote_ctx->host = strdup(remote_host);
	remote_ctx->port = port;
	remote_ctx->keep_alive = keep_alive;

	evl = evconnlistener_new_bind(base, forward_local_accept, remote_ctx,
		LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC |
		LEV_OPT_REUSEABLE | LEV_OPT_DEFERRED_ACCEPT, 10,
		result->ai_addr, result->ai_addrlen);

	freeaddrinfo(result);

	if (!evl) {
		free(remote_ctx->host);
		free(remote_ctx);
		perror(__func__);
		return -1;
	}

	return 0;
}
