/*
 * Copyright (c) 2016, lvsgate@163.com
 * All rights reserved.
 *
 * SPDX-License-Identifier:	BSD-3-Clause
 */

#include <getopt.h>
#include <string.h>
#include <signal.h>

#include "ofp.h"

#include "ofp_vs.h"

uint64_t rte_hz;

const char *ip_vs_proto_name(unsigned proto)
{
	static char buf[20];

	switch (proto) {
	case IPPROTO_IP:
		return "IP";
	case IPPROTO_UDP:
		return "UDP";
	case IPPROTO_TCP:
		return "TCP";
	case IPPROTO_ICMP:
		return "ICMP";
#ifdef CONFIG_IP_VS_IPV6
	case IPPROTO_ICMPV6:
		return "ICMPv6";
#endif
	default:
		sprintf(buf, "IP_%d", proto);
		return buf;
	}
}


/*
 *	Handle ICMP messages in the outside-to-inside direction (incoming).
 *	Find any that might be relevant, check against existing connections,
 *	forward to the right destination host if relevant.
 *	Currently handles error types - unreachable, quench, ttl exceeded.
 */

static inline int
ip_vs_set_state(struct ip_vs_conn *cp, int direction,
		const struct rte_mbuf *skb, struct ip_vs_protocol *pp)
{
	if (unlikely(!pp->state_transition))
		return 0;
	return pp->state_transition(cp, direction, skb, pp);
}


static int
ip_vs_in_icmp(struct rte_mbuf *skb, int *related)
{
	*related = 0;
	return NF_ACCEPT;
}



/* Handle response packets: rewrite addresses and send away...
 * Used for NAT / local client / FULLNAT.
 */
static inline unsigned int
handle_response(int af, struct rte_mbuf *skb, struct ip_vs_protocol *pp,
		struct ip_vs_conn *cp, int ihl)
{
	int ret = NF_DROP;

	/* statistics */
	ip_vs_out_stats(cp, skb);

	/*
	 * Syn-proxy step 3 logic: receive syn-ack from rs.
	 */
	/*
	if ((cp->flags & IP_VS_CONN_F_SYNPROXY) &&
		(cp->state == IP_VS_TCP_S_SYNPROXY) &&
		(ip_vs_synproxy_synack_rcv(skb, cp, pp, ihl, &ret) == 0)) {
		goto out;
	}
	*/

	/* state transition */
	ip_vs_set_state(cp, IP_VS_DIR_OUTPUT, skb, pp);
	/* transmit */

	if (cp->flags & IP_VS_CONN_F_FULLNAT) {
		{
			ret = ip_vs_fnat_response_xmit(skb, pp, cp, ihl);
		}
	} else {
		{
			//ret = ip_vs_normal_response_xmit(skb, pp, cp, ihl);
		}
	}

out:
	ip_vs_conn_put(cp);
	return ret;
}

static inline __u16
ip_vs_onepacket_enabled(struct ip_vs_service *svc, struct ip_vs_iphdr *iph)
{
	return (svc->flags & IP_VS_SVC_F_ONEPACKET
		&& iph->protocol == IPPROTO_UDP)
	    ? IP_VS_CONN_F_ONE_PACKET : 0;
}

/*
 *  IPVS main scheduling function
 *  It selects a server according to the virtual service, and
 *  creates a connection entry.
 *  Protocols supported: TCP, UDP
 */
struct ip_vs_conn *ip_vs_schedule(struct ip_vs_service *svc,
				  struct rte_mbuf *skb, int is_synproxy_on)
{
	struct ip_vs_conn *cp = NULL;
	struct ip_vs_iphdr iph;
	struct ip_vs_dest *dest;
	__be16 _ports[2], *pptr;

	ip_vs_fill_iphdr(svc->af, ip_hdr(skb), &iph);
	pptr = rte_pktmbuf_mtod_offset(skb, __be16 *,
			sizeof(struct ether_hdr) + iph.len);
	if (pptr == NULL)
		return NULL;

	/*
	 *    Persistent service
	 */
	/*
	if (svc->flags & IP_VS_SVC_F_PERSISTENT)
		return ip_vs_sched_persist(svc, skb, pptr, is_synproxy_on);
	*/

	/*
	 *    Non-persistent service
	 */
	if (!svc->fwmark && pptr[1] != svc->port) {
		if (!svc->port)
			pr_err("Schedule: port zero only supported "
			       "in persistent services, "
			       "check your ipvs configuration\n");
		return NULL;
	}

	dest = svc->scheduler->schedule(svc, skb);
	if (dest == NULL) {
		IP_VS_DBG(1, "Schedule: no dest found.\n");
		return NULL;
	}

	/*
	 *    Create a connection entry.
	 */
	cp = ip_vs_conn_new(svc->af, iph.protocol,
			    &iph.saddr, pptr[0],
			    &iph.daddr, pptr[1],
			    &dest->addr, dest->port ? dest->port : pptr[1],
			    ip_vs_onepacket_enabled(svc, &iph),
			    dest, skb, is_synproxy_on);
	if (cp == NULL)
		return NULL;

	IP_VS_DBG_BUF(6, "Schedule fwd:%c c:%s:%u v:%s:%u "
		      "d:%s:%u conn->flags:%X conn->refcnt:%d cpu%d\n",
		      ip_vs_fwd_tag(cp),
		      IP_VS_DBG_ADDR(svc->af, &cp->caddr), ntohs(cp->cport),
		      IP_VS_DBG_ADDR(svc->af, &cp->vaddr), ntohs(cp->vport),
		      IP_VS_DBG_ADDR(svc->af, &cp->daddr), ntohs(cp->dport),
		      cp->flags, atomic_read(&cp->refcnt), cp->cpuid);

	ip_vs_conn_stats(cp, svc);
	return cp;
}

/*
 *  Pass or drop the packet.
 *  Called by ip_vs_in, when the virtual service is available but
 *  no destination is available for a new connection.
 */
int ip_vs_leave(struct ip_vs_service *svc, struct rte_mbuf *skb,
		struct ip_vs_protocol *pp)
{
	__be16 _ports[2], *pptr;
	struct ip_vs_iphdr iph;
	int unicast;
	ip_vs_fill_iphdr(svc->af, ip_hdr(skb), &iph);

	pptr = rte_pktmbuf_mtod_offset(skb, __be16 *,
			sizeof(struct ether_hdr) + iph.len);
	if (pptr == NULL) {
		ip_vs_service_put(svc);
		return NF_DROP;
	}

	/*
	 * When the virtual ftp service is presented, packets destined
	 * for other services on the VIP may get here (except services
	 * listed in the ipvs table), pass the packets, because it is
	 * not ipvs job to decide to drop the packets.
	 */
	if ((svc->port == FTPPORT) && (pptr[1] != FTPPORT)) {
		ip_vs_service_put(svc);
		return NF_ACCEPT;
	}

	ip_vs_service_put(svc);

	/*
	 * Notify the client that the destination is unreachable, and
	 * release the socket buffer.
	 * Since it is in IP layer, the TCP socket is not actually
	 * created, the TCP RST packet cannot be sent, instead that
	 * ICMP_PORT_UNREACH is sent here no matter it is TCP/UDP. --WZ
	 */
#ifdef CONFIG_IP_VS_IPV6
	if (svc->af == AF_INET6)
		icmpv6_send(skb, ICMPV6_DEST_UNREACH, ICMPV6_PORT_UNREACH, 0,
			    skb->dev);
	else
#endif
		//icmp_send(skb, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH, 0);

	IP_VS_INC_ESTATS(ip_vs_esmib, CONN_SCHED_UNREACH);

	return NF_DROP;
}

enum ofp_return_code ofp_vs_in(odp_packet_t pkt, void *arg)
{
	int protocol = *(int *)arg;
	struct ether_hdr *eth_hdr;
	struct rte_mbuf *skb = (struct rte_mbuf *)pkt;
	struct iphdr *iphdr;
	struct ip_vs_iphdr iph;
	struct ip_vs_protocol *pp;
	struct ip_vs_conn *cp;
	int ret, restart, af, pkts;
	int v = NF_DROP;
	int res_dir;
	int tot_len;

	eth_hdr = rte_pktmbuf_mtod(skb, struct ether_hdr*);
	
	/* Only support IPV4 */
    	if(!RTE_ETH_IS_IPV4_HDR(skb->packet_type))
		return NF_ACCEPT;

	af = AF_INET;
	iphdr = rte_pktmbuf_mtod_offset(skb, struct iphdr *,
					   sizeof(struct ether_hdr));

	tot_len = rte_be_to_cpu_16(iphdr->tot_len);
	if (tot_len > skb->data_len || tot_len < ip_hdrlen(iphdr)) {
		return NF_DROP;
	}

	ip_vs_fill_iphdr(af, iphdr, &iph);

	if (unlikely(iph.protocol == IPPROTO_ICMP)) {
		int related, verdict = ip_vs_in_icmp(skb, &related);

		if (related)
			return verdict;
		ip_vs_fill_iphdr(af, iphdr, &iph);
	}

	
	/* Protocol supported? */
	pp = ip_vs_proto_get(iph.protocol);
	if (unlikely(!pp))
		return NF_ACCEPT;

	/*
	 * Check if the packet belongs to an existing connection entry
	 */
	cp = pp->conn_in_get(af, skb, pp, &iph, iph.len, 0, &res_dir);

	if (likely(cp)) {
		/* For full-nat/local-client packets, it could be a response */
		if (res_dir == IP_VS_CIDX_F_IN2OUT) {
			return handle_response(af, skb, pp, cp, iph.len);
		}
	} else {
		/* create a new connection */
		int v;

		if (!pp->conn_schedule(af, skb, pp, &v, &cp))
			return v;
	}

	if (unlikely(!cp)) {
		/* sorry, all this trouble for a no-hit :) */
		IP_VS_DBG_PKT(12, pp, skb, 0,
			      "packet continues traversal as normal");
		return NF_ACCEPT;
	}

	IP_VS_DBG_PKT(11, pp, skb, 0, "Incoming packet");

	/* Check the server status */
	if (cp->dest && !(cp->dest->flags & IP_VS_DEST_F_AVAILABLE)) {
		/* the destination server is not available */

		if (sysctl_ip_vs_expire_nodest_conn) {
			/* try to expire the connection immediately */
			ip_vs_conn_expire_now(cp);
		}
		/* don't restart its timer, and silently
		   drop the packet. */
		__ip_vs_conn_put(cp);
		return NF_DROP;
	}

	ip_vs_in_stats(cp, skb);

	/*
	 * Filter out-in ack packet when cp is at SYN_SENT state.
	 * DROP it if not a valid packet, STORE it if we have 
	 * space left. 
	 */
	/*
	if ((cp->flags & IP_VS_CONN_F_SYNPROXY) &&
	    (0 == ip_vs_synproxy_filter_ack(skb, cp, pp, &iph, &v))) {
		ip_vs_conn_put(cp);
		return v;
	}
	*/

	/*
	 * "Reuse" syn-proxy sessions.
	 * "Reuse" means update syn_proxy_seq struct and clean ack_skb etc.
	 */
	/*
	if ((cp->flags & IP_VS_CONN_F_SYNPROXY) &&
	    (0 != sysctl_ip_vs_synproxy_conn_reuse)) {
		int v = NF_DROP;

		if (0 == ip_vs_synproxy_reuse_conn(af, skb, cp, pp, &iph, &v)) {
			ip_vs_conn_put(cp);
			return v;
		}
	}
	*/

	restart = ip_vs_set_state(cp, IP_VS_DIR_INPUT, skb, pp);
	if (cp->packet_xmit)
		ret = cp->packet_xmit(skb, cp, pp);
	/* do not touch skb anymore */
	else {
		IP_VS_DBG_RL("warning: packet_xmit is null");
		ret = NF_ACCEPT;
	}

	/* Increase its packet counter and check if it is needed
	 * to be synchronized
	 *
	 * Sync connection if it is about to close to
	 * encorage the standby servers to update the connections timeout
	 */
	pkts = atomic_add_return(1, &cp->in_pkts);
	/*
	if (af == AF_INET &&
	    (ip_vs_sync_state & IP_VS_STATE_MASTER) &&
	    (((cp->protocol != IPPROTO_TCP ||
	       cp->state == IP_VS_TCP_S_ESTABLISHED) &&
	      (pkts % sysctl_ip_vs_sync_threshold[1]
	       == sysctl_ip_vs_sync_threshold[0])) ||
	     ((cp->protocol == IPPROTO_TCP) && (cp->old_state != cp->state) &&
	      ((cp->state == IP_VS_TCP_S_FIN_WAIT) ||
	       (cp->state == IP_VS_TCP_S_CLOSE_WAIT) ||
	       (cp->state == IP_VS_TCP_S_TIME_WAIT)))))
		ip_vs_sync_conn(cp);
	*/
	cp->old_state = cp->state;

	ip_vs_conn_put(cp);
	return ret;
}

int ofp_vs_init(odp_instance_t instance, ofp_init_global_t *app_init_params)
{
	int ret;
	
	rte_hz = rte_get_timer_hz();

	if ((ret = ofp_vs_ctl_init(instance, app_init_params)) < 0)
		return ret;

	if ((ret = ip_vs_protocol_init() < 0))
		return ret;


	if ((ret = ip_vs_conn_init()) < 0)
		return ret;

	
	if ((ret = ip_vs_rr_init()) < 0)
		return ret;


	return ret;
}

void ofp_vs_finish(void)
{
	ip_vs_rr_cleanup();
	ip_vs_protocol_cleanup();
	ip_vs_conn_cleanup();
	ofp_vs_ctl_finish();
}