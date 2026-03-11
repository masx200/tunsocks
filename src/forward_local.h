#ifndef __FORWARD_LOCAL_H__
#define __FORWARD_LOCAL_H__

struct event_base;

int forward_local(struct event_base *base, const char *local_host, const char *local_port,
	const char *remote_host, const char *remote_port, int keep_alive, int is_udp);

#endif
