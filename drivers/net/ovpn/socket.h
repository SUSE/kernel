/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2025 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_SOCK_H_
#define _NET_OVPN_SOCK_H_

#include <linux/net.h>
#include <linux/kref.h>
#include <net/sock.h>

struct ovpn_priv;
struct ovpn_peer;

/**
 * struct ovpn_socket - a kernel socket referenced in the ovpn code
 * @ovpn: ovpn instance owning this socket (UDP only)
 * @dev_tracker: reference tracker for associated dev (UDP only)
 * @sock: the low level sock object
 * @refcount: amount of contexts currently referencing this object
 * @rcu: member used to schedule RCU destructor callback
 */
struct ovpn_socket {
	union {
		struct {
			struct ovpn_priv *ovpn;
			netdevice_tracker dev_tracker;
		};
	};

	struct socket *sock;
	struct kref refcount;
	struct rcu_head rcu;
};

struct ovpn_socket *ovpn_socket_new(struct socket *sock,
				    struct ovpn_peer *peer);
void ovpn_socket_release(struct ovpn_peer *peer);

#endif /* _NET_OVPN_SOCK_H_ */
