// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/if_vlan.h>
#include <linux/dsa/sja1105.h>
#include <linux/dsa/8021q.h>
#include <linux/packing.h>
#include "dsa_priv.h"

/* Is this a TX or an RX header? */
#define SJA1110_HEADER_HOST_TO_SWITCH		BIT(15)

/* RX header */
#define SJA1110_RX_HEADER_IS_METADATA		BIT(14)
#define SJA1110_RX_HEADER_HOST_ONLY		BIT(13)
#define SJA1110_RX_HEADER_HAS_TRAILER		BIT(12)

/* Trap-to-host format (no trailer present) */
#define SJA1110_RX_HEADER_SRC_PORT(x)		(((x) & GENMASK(7, 4)) >> 4)
#define SJA1110_RX_HEADER_SWITCH_ID(x)		((x) & GENMASK(3, 0))

/* Timestamp format (trailer present) */
#define SJA1110_RX_HEADER_TRAILER_POS(x)	((x) & GENMASK(11, 0))

#define SJA1110_RX_TRAILER_SWITCH_ID(x)		(((x) & GENMASK(7, 4)) >> 4)
#define SJA1110_RX_TRAILER_SRC_PORT(x)		((x) & GENMASK(3, 0))

/* Meta frame format (for 2-step TX timestamps) */
#define SJA1110_RX_HEADER_N_TS(x)		(((x) & GENMASK(8, 4)) >> 4)

/* TX header */
#define SJA1110_TX_HEADER_UPDATE_TC		BIT(14)
#define SJA1110_TX_HEADER_TAKE_TS		BIT(13)
#define SJA1110_TX_HEADER_TAKE_TS_CASC		BIT(12)
#define SJA1110_TX_HEADER_HAS_TRAILER		BIT(11)

/* Only valid if SJA1110_TX_HEADER_HAS_TRAILER is false */
#define SJA1110_TX_HEADER_PRIO(x)		(((x) << 7) & GENMASK(10, 7))
#define SJA1110_TX_HEADER_TSTAMP_ID(x)		((x) & GENMASK(7, 0))

/* Only valid if SJA1110_TX_HEADER_HAS_TRAILER is true */
#define SJA1110_TX_HEADER_TRAILER_POS(x)	((x) & GENMASK(10, 0))

#define SJA1110_TX_TRAILER_TSTAMP_ID(x)		(((x) << 24) & GENMASK(31, 24))
#define SJA1110_TX_TRAILER_PRIO(x)		(((x) << 21) & GENMASK(23, 21))
#define SJA1110_TX_TRAILER_SWITCHID(x)		(((x) << 12) & GENMASK(15, 12))
#define SJA1110_TX_TRAILER_DESTPORTS(x)		(((x) << 1) & GENMASK(11, 1))

#define SJA1110_META_TSTAMP_SIZE		10

#define SJA1110_HEADER_LEN			4
#define SJA1110_RX_TRAILER_LEN			13
#define SJA1110_TX_TRAILER_LEN			4
#define SJA1110_MAX_PADDING_LEN			15

/* Similar to is_link_local_ether_addr(hdr->h_dest) but also covers PTP */
static inline bool sja1105_is_link_local(const struct sk_buff *skb)
{
	const struct ethhdr *hdr = eth_hdr(skb);
	u64 dmac = ether_addr_to_u64(hdr->h_dest);

	if (ntohs(hdr->h_proto) == ETH_P_SJA1105_META)
		return false;
	if ((dmac & SJA1105_LINKLOCAL_FILTER_A_MASK) ==
		    SJA1105_LINKLOCAL_FILTER_A)
		return true;
	if ((dmac & SJA1105_LINKLOCAL_FILTER_B_MASK) ==
		    SJA1105_LINKLOCAL_FILTER_B)
		return true;
	return false;
}

struct sja1105_meta {
	u64 tstamp;
	u64 dmac_byte_4;
	u64 dmac_byte_3;
	u64 source_port;
	u64 switch_id;
};

static void sja1105_meta_unpack(const struct sk_buff *skb,
				struct sja1105_meta *meta)
{
	u8 *buf = skb_mac_header(skb) + ETH_HLEN;

	/* UM10944.pdf section 4.2.17 AVB Parameters:
	 * Structure of the meta-data follow-up frame.
	 * It is in network byte order, so there are no quirks
	 * while unpacking the meta frame.
	 *
	 * Also SJA1105 E/T only populates bits 23:0 of the timestamp
	 * whereas P/Q/R/S does 32 bits. Since the structure is the
	 * same and the E/T puts zeroes in the high-order byte, use
	 * a unified unpacking command for both device series.
	 */
	packing(buf,     &meta->tstamp,     31, 0, 4, UNPACK, 0);
	packing(buf + 4, &meta->dmac_byte_4, 7, 0, 1, UNPACK, 0);
	packing(buf + 5, &meta->dmac_byte_3, 7, 0, 1, UNPACK, 0);
	packing(buf + 6, &meta->source_port, 7, 0, 1, UNPACK, 0);
	packing(buf + 7, &meta->switch_id,   7, 0, 1, UNPACK, 0);
}

static inline bool sja1105_is_meta_frame(const struct sk_buff *skb)
{
	const struct ethhdr *hdr = eth_hdr(skb);
	u64 smac = ether_addr_to_u64(hdr->h_source);
	u64 dmac = ether_addr_to_u64(hdr->h_dest);

	if (smac != SJA1105_META_SMAC)
		return false;
	if (dmac != SJA1105_META_DMAC)
		return false;
	if (ntohs(hdr->h_proto) != ETH_P_SJA1105_META)
		return false;
	return true;
}

static bool sja1105_can_use_vlan_as_tags(const struct sk_buff *skb)
{
	struct vlan_ethhdr *hdr = vlan_eth_hdr(skb);
	u16 vlan_tci;

	if (hdr->h_vlan_proto == htons(ETH_P_SJA1105))
		return true;

	if (hdr->h_vlan_proto != htons(ETH_P_8021Q) &&
	    !skb_vlan_tag_present(skb))
		return false;

	if (skb_vlan_tag_present(skb))
		vlan_tci = skb_vlan_tag_get(skb);
	else
		vlan_tci = ntohs(hdr->h_vlan_TCI);

	return vid_is_dsa_8021q(vlan_tci & VLAN_VID_MASK);
}

/* This is the first time the tagger sees the frame on RX.
 * Figure out if we can decode it.
 */
static bool sja1105_filter(const struct sk_buff *skb, struct net_device *dev)
{
	if (sja1105_can_use_vlan_as_tags(skb))
		return true;
	if (sja1105_is_link_local(skb))
		return true;
	if (sja1105_is_meta_frame(skb))
		return true;
	return false;
}

/* Calls sja1105_port_deferred_xmit in sja1105_main.c */
static struct sk_buff *sja1105_defer_xmit(struct sja1105_port *sp,
					  struct sk_buff *skb)
{
	/* Increase refcount so the kfree_skb in dsa_slave_xmit
	 * won't really free the packet.
	 */
	skb_queue_tail(&sp->xmit_queue, skb_get(skb));
	kthread_queue_work(sp->xmit_worker, &sp->xmit_work);

	return NULL;
}

static u16 sja1105_xmit_tpid(struct sja1105_port *sp)
{
	return sp->xmit_tpid;
}

static struct sk_buff *sja1105_xmit(struct sk_buff *skb,
				    struct net_device *netdev)
{
	struct dsa_port *dp = dsa_slave_to_port(netdev);
	u16 tx_vid = dsa_8021q_tx_vid(dp->ds, dp->index);
	u16 queue_mapping = skb_get_queue_mapping(skb);
	u8 pcp = netdev_txq_to_tc(netdev, queue_mapping);

	/* Transmitting management traffic does not rely upon switch tagging,
	 * but instead SPI-installed management routes. Part 2 of this
	 * is the .port_deferred_xmit driver callback.
	 */
	if (unlikely(sja1105_is_link_local(skb)))
		return sja1105_defer_xmit(dp->priv, skb);

	return dsa_8021q_xmit(skb, netdev, sja1105_xmit_tpid(dp->priv),
			     ((pcp << VLAN_PRIO_SHIFT) | tx_vid));
}

static struct sk_buff *sja1110_xmit(struct sk_buff *skb,
				    struct net_device *netdev)
{
	struct sk_buff *clone = SJA1105_SKB_CB(skb)->clone;
	struct dsa_port *dp = dsa_slave_to_port(netdev);
	u16 tx_vid = dsa_8021q_tx_vid(dp->ds, dp->index);
	u16 queue_mapping = skb_get_queue_mapping(skb);
	u8 pcp = netdev_txq_to_tc(netdev, queue_mapping);
	struct ethhdr *eth_hdr;
	__be32 *tx_trailer;
	__be16 *tx_header;
	int trailer_pos;

	/* Transmitting control packets is done using in-band control
	 * extensions, while data packets are transmitted using
	 * tag_8021q TX VLANs.
	 */
	if (likely(!sja1105_is_link_local(skb)))
		return dsa_8021q_xmit(skb, netdev, sja1105_xmit_tpid(dp->priv),
				     ((pcp << VLAN_PRIO_SHIFT) | tx_vid));

	skb_push(skb, SJA1110_HEADER_LEN);

	/* Move Ethernet header to the left, making space for DSA tag */
	memmove(skb->data, skb->data + SJA1110_HEADER_LEN, 2 * ETH_ALEN);

	trailer_pos = skb->len;

	/* On TX, skb->data points to skb_mac_header(skb) */
	eth_hdr = (struct ethhdr *)skb->data;
	tx_header = (__be16 *)(eth_hdr + 1);
	tx_trailer = skb_put(skb, SJA1110_TX_TRAILER_LEN);

	eth_hdr->h_proto = htons(ETH_P_SJA1110);

	*tx_header = htons(SJA1110_HEADER_HOST_TO_SWITCH |
			   SJA1110_TX_HEADER_HAS_TRAILER |
			   SJA1110_TX_HEADER_TRAILER_POS(trailer_pos));
	*tx_trailer = cpu_to_be32(SJA1110_TX_TRAILER_PRIO(pcp) |
				  SJA1110_TX_TRAILER_SWITCHID(dp->ds->index) |
				  SJA1110_TX_TRAILER_DESTPORTS(BIT(dp->index)));
	if (clone) {
		u8 ts_id = SJA1105_SKB_CB(clone)->ts_id;

		*tx_header |= htons(SJA1110_TX_HEADER_TAKE_TS);
		*tx_trailer |= cpu_to_be32(SJA1110_TX_TRAILER_TSTAMP_ID(ts_id));
	}

	return skb;
}

static void sja1105_transfer_meta(struct sk_buff *skb,
				  const struct sja1105_meta *meta)
{
	struct ethhdr *hdr = eth_hdr(skb);

	hdr->h_dest[3] = meta->dmac_byte_3;
	hdr->h_dest[4] = meta->dmac_byte_4;
	SJA1105_SKB_CB(skb)->tstamp = meta->tstamp;
}

/* This is a simple state machine which follows the hardware mechanism of
 * generating RX timestamps:
 *
 * After each timestampable skb (all traffic for which send_meta1 and
 * send_meta0 is true, aka all MAC-filtered link-local traffic) a meta frame
 * containing a partial timestamp is immediately generated by the switch and
 * sent as a follow-up to the link-local frame on the CPU port.
 *
 * The meta frames have no unique identifier (such as sequence number) by which
 * one may pair them to the correct timestampable frame.
 * Instead, the switch has internal logic that ensures no frames are sent on
 * the CPU port between a link-local timestampable frame and its corresponding
 * meta follow-up. It also ensures strict ordering between ports (lower ports
 * have higher priority towards the CPU port). For this reason, a per-port
 * data structure is not needed/desirable.
 *
 * This function pairs the link-local frame with its partial timestamp from the
 * meta follow-up frame. The full timestamp will be reconstructed later in a
 * work queue.
 */
static struct sk_buff
*sja1105_rcv_meta_state_machine(struct sk_buff *skb,
				struct sja1105_meta *meta,
				bool is_link_local,
				bool is_meta)
{
	struct sja1105_port *sp;
	struct dsa_port *dp;

	dp = dsa_slave_to_port(skb->dev);
	sp = dp->priv;

	/* Step 1: A timestampable frame was received.
	 * Buffer it until we get its meta frame.
	 */
	if (is_link_local) {
		if (!test_bit(SJA1105_HWTS_RX_EN, &sp->data->state))
			/* Do normal processing. */
			return skb;

		spin_lock(&sp->data->meta_lock);
		/* Was this a link-local frame instead of the meta
		 * that we were expecting?
		 */
		if (sp->data->stampable_skb) {
			dev_err_ratelimited(dp->ds->dev,
					    "Expected meta frame, is %12llx "
					    "in the DSA master multicast filter?\n",
					    SJA1105_META_DMAC);
			kfree_skb(sp->data->stampable_skb);
		}

		/* Hold a reference to avoid dsa_switch_rcv
		 * from freeing the skb.
		 */
		sp->data->stampable_skb = skb_get(skb);
		spin_unlock(&sp->data->meta_lock);

		/* Tell DSA we got nothing */
		return NULL;

	/* Step 2: The meta frame arrived.
	 * Time to take the stampable skb out of the closet, annotate it
	 * with the partial timestamp, and pretend that we received it
	 * just now (basically masquerade the buffered frame as the meta
	 * frame, which serves no further purpose).
	 */
	} else if (is_meta) {
		struct sk_buff *stampable_skb;

		/* Drop the meta frame if we're not in the right state
		 * to process it.
		 */
		if (!test_bit(SJA1105_HWTS_RX_EN, &sp->data->state))
			return NULL;

		spin_lock(&sp->data->meta_lock);

		stampable_skb = sp->data->stampable_skb;
		sp->data->stampable_skb = NULL;

		/* Was this a meta frame instead of the link-local
		 * that we were expecting?
		 */
		if (!stampable_skb) {
			dev_err_ratelimited(dp->ds->dev,
					    "Unexpected meta frame\n");
			spin_unlock(&sp->data->meta_lock);
			return NULL;
		}

		if (stampable_skb->dev != skb->dev) {
			dev_err_ratelimited(dp->ds->dev,
					    "Meta frame on wrong port\n");
			spin_unlock(&sp->data->meta_lock);
			return NULL;
		}

		/* Free the meta frame and give DSA the buffered stampable_skb
		 * for further processing up the network stack.
		 */
		kfree_skb(skb);
		skb = stampable_skb;
		sja1105_transfer_meta(skb, meta);

		spin_unlock(&sp->data->meta_lock);
	}

	return skb;
}

static bool sja1105_skb_has_tag_8021q(const struct sk_buff *skb)
{
	u16 tpid = ntohs(eth_hdr(skb)->h_proto);

	return tpid == ETH_P_SJA1105 || tpid == ETH_P_8021Q ||
	       skb_vlan_tag_present(skb);
}

static bool sja1110_skb_has_inband_control_extension(const struct sk_buff *skb)
{
	return ntohs(eth_hdr(skb)->h_proto) == ETH_P_SJA1110;
}

static struct sk_buff *sja1105_rcv(struct sk_buff *skb,
				   struct net_device *netdev,
				   struct packet_type *pt)
{
	struct sja1105_meta meta = {0};
	int source_port, switch_id;
	struct ethhdr *hdr;
	bool is_link_local;
	bool is_meta;

	hdr = eth_hdr(skb);
	is_link_local = sja1105_is_link_local(skb);
	is_meta = sja1105_is_meta_frame(skb);

	skb->offload_fwd_mark = 1;

	if (sja1105_skb_has_tag_8021q(skb)) {
		/* Normal traffic path. */
		dsa_8021q_rcv(skb, &source_port, &switch_id);
	} else if (is_link_local) {
		/* Management traffic path. Switch embeds the switch ID and
		 * port ID into bytes of the destination MAC, courtesy of
		 * the incl_srcpt options.
		 */
		source_port = hdr->h_dest[3];
		switch_id = hdr->h_dest[4];
		/* Clear the DMAC bytes that were mangled by the switch */
		hdr->h_dest[3] = 0;
		hdr->h_dest[4] = 0;
	} else if (is_meta) {
		sja1105_meta_unpack(skb, &meta);
		source_port = meta.source_port;
		switch_id = meta.switch_id;
	} else {
		return NULL;
	}

	skb->dev = dsa_master_find_slave(netdev, switch_id, source_port);
	if (!skb->dev) {
		netdev_warn(netdev, "Couldn't decode source port\n");
		return NULL;
	}

	return sja1105_rcv_meta_state_machine(skb, &meta, is_link_local,
					      is_meta);
}

static struct sk_buff *sja1110_rcv_meta(struct sk_buff *skb, u16 rx_header)
{
	int switch_id = SJA1110_RX_HEADER_SWITCH_ID(rx_header);
	int n_ts = SJA1110_RX_HEADER_N_TS(rx_header);
	struct net_device *master = skb->dev;
	struct dsa_port *cpu_dp;
	u8 *buf = skb->data + 2;
	struct dsa_switch *ds;
	int i;

	cpu_dp = master->dsa_ptr;
	ds = dsa_switch_find(cpu_dp->dst->index, switch_id);
	if (!ds) {
		net_err_ratelimited("%s: cannot find switch id %d\n",
				    master->name, switch_id);
		return NULL;
	}

	for (i = 0; i <= n_ts; i++) {
		u8 ts_id, source_port, dir;
		u64 tstamp;

		ts_id = buf[0];
		source_port = (buf[1] & GENMASK(7, 4)) >> 4;
		dir = (buf[1] & BIT(3)) >> 3;
		tstamp = be64_to_cpu(*(__be64 *)(buf + 2));

		sja1110_process_meta_tstamp(ds, source_port, ts_id, dir,
					    tstamp);

		buf += SJA1110_META_TSTAMP_SIZE;
	}

	/* Discard the meta frame, we've consumed the timestamps it contained */
	return NULL;
}

static struct sk_buff *sja1110_rcv_inband_control_extension(struct sk_buff *skb,
							    int *source_port,
							    int *switch_id)
{
	u16 rx_header;

	if (unlikely(!pskb_may_pull(skb, SJA1110_HEADER_LEN)))
		return NULL;

	/* skb->data points to skb_mac_header(skb) + ETH_HLEN, which is exactly
	 * what we need because the caller has checked the EtherType (which is
	 * located 2 bytes back) and we just need a pointer to the header that
	 * comes afterwards.
	 */
	rx_header = ntohs(*(__be16 *)skb->data);

	if (rx_header & SJA1110_RX_HEADER_IS_METADATA)
		return sja1110_rcv_meta(skb, rx_header);

	/* Timestamp frame, we have a trailer */
	if (rx_header & SJA1110_RX_HEADER_HAS_TRAILER) {
		int start_of_padding = SJA1110_RX_HEADER_TRAILER_POS(rx_header);
		u8 *rx_trailer = skb_tail_pointer(skb) - SJA1110_RX_TRAILER_LEN;
		u64 *tstamp = &SJA1105_SKB_CB(skb)->tstamp;
		u8 last_byte = rx_trailer[12];

		/* The timestamp is unaligned, so we need to use packing()
		 * to get it
		 */
		packing(rx_trailer, tstamp, 63, 0, 8, UNPACK, 0);

		*source_port = SJA1110_RX_TRAILER_SRC_PORT(last_byte);
		*switch_id = SJA1110_RX_TRAILER_SWITCH_ID(last_byte);

		/* skb->len counts from skb->data, while start_of_padding
		 * counts from the destination MAC address. Right now skb->data
		 * is still as set by the DSA master, so to trim away the
		 * padding and trailer we need to account for the fact that
		 * skb->data points to skb_mac_header(skb) + ETH_HLEN.
		 */
		pskb_trim_rcsum(skb, start_of_padding - ETH_HLEN);
	/* Trap-to-host frame, no timestamp trailer */
	} else {
		*source_port = SJA1110_RX_HEADER_SRC_PORT(rx_header);
		*switch_id = SJA1110_RX_HEADER_SWITCH_ID(rx_header);
	}

	/* Advance skb->data past the DSA header */
	skb_pull_rcsum(skb, SJA1110_HEADER_LEN);

	/* Remove the DSA header */
	memmove(skb->data - ETH_HLEN, skb->data - ETH_HLEN - SJA1110_HEADER_LEN,
		2 * ETH_ALEN);

	/* With skb->data in its final place, update the MAC header
	 * so that eth_hdr() continues to works properly.
	 */
	skb_set_mac_header(skb, -ETH_HLEN);

	return skb;
}

static struct sk_buff *sja1110_rcv(struct sk_buff *skb,
				   struct net_device *netdev,
				   struct packet_type *pt)
{
	int source_port = -1, switch_id = -1;

	skb->offload_fwd_mark = 1;

	if (sja1110_skb_has_inband_control_extension(skb)) {
		skb = sja1110_rcv_inband_control_extension(skb, &source_port,
							   &switch_id);
		if (!skb)
			return NULL;
	}

	/* Packets with in-band control extensions might still have RX VLANs */
	if (likely(sja1105_skb_has_tag_8021q(skb)))
		dsa_8021q_rcv(skb, &source_port, &switch_id);

	skb->dev = dsa_master_find_slave(netdev, switch_id, source_port);
	if (!skb->dev) {
		netdev_warn(netdev,
			    "Couldn't decode source port %d and switch id %d\n",
			    source_port, switch_id);
		return NULL;
	}

	return skb;
}

static void sja1105_flow_dissect(const struct sk_buff *skb, __be16 *proto,
				 int *offset)
{
	/* No tag added for management frames, all ok */
	if (unlikely(sja1105_is_link_local(skb)))
		return;

	dsa_tag_generic_flow_dissect(skb, proto, offset);
}

static void sja1110_flow_dissect(const struct sk_buff *skb, __be16 *proto,
				 int *offset)
{
	/* Management frames have 2 DSA tags on RX, so the needed_headroom we
	 * declared is fine for the generic dissector adjustment procedure.
	 */
	if (unlikely(sja1105_is_link_local(skb)))
		return dsa_tag_generic_flow_dissect(skb, proto, offset);

	/* For the rest, there is a single DSA tag, the tag_8021q one */
	*offset = VLAN_HLEN;
	*proto = ((__be16 *)skb->data)[(VLAN_HLEN / 2) - 1];
}

static const struct dsa_device_ops sja1105_netdev_ops = {
	.name = "sja1105",
	.proto = DSA_TAG_PROTO_SJA1105,
	.xmit = sja1105_xmit,
	.rcv = sja1105_rcv,
	.filter = sja1105_filter,
	.needed_headroom = VLAN_HLEN,
	.flow_dissect = sja1105_flow_dissect,
	.promisc_on_master = true,
};

DSA_TAG_DRIVER(sja1105_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_SJA1105);

static const struct dsa_device_ops sja1110_netdev_ops = {
	.name = "sja1110",
	.proto = DSA_TAG_PROTO_SJA1110,
	.xmit = sja1110_xmit,
	.rcv = sja1110_rcv,
	.filter = sja1105_filter,
	.flow_dissect = sja1110_flow_dissect,
	.needed_headroom = SJA1110_HEADER_LEN + VLAN_HLEN,
	.needed_tailroom = SJA1110_RX_TRAILER_LEN + SJA1110_MAX_PADDING_LEN,
};

DSA_TAG_DRIVER(sja1110_netdev_ops);
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_SJA1110);

static struct dsa_tag_driver *sja1105_tag_driver_array[] = {
	&DSA_TAG_DRIVER_NAME(sja1105_netdev_ops),
	&DSA_TAG_DRIVER_NAME(sja1110_netdev_ops),
};

module_dsa_tag_drivers(sja1105_tag_driver_array);

MODULE_LICENSE("GPL v2");
