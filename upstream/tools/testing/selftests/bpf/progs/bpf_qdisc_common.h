/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _BPF_QDISC_COMMON_H
#define _BPF_QDISC_COMMON_H

#define NET_XMIT_SUCCESS        0x00
#define NET_XMIT_DROP           0x01    /* skb dropped                  */
#define NET_XMIT_CN             0x02    /* congestion notification      */

#define TC_PRIO_CONTROL  7
#define TC_PRIO_MAX      15

#define private(name) SEC(".data." #name) __hidden __attribute__((aligned(8)))

u32 bpf_skb_get_hash(struct sk_buff *p) __ksym;
void bpf_kfree_skb(struct sk_buff *p) __ksym;
void bpf_qdisc_skb_drop(struct sk_buff *p, struct bpf_sk_buff_ptr *to_free) __ksym;
void bpf_qdisc_watchdog_schedule(struct Qdisc *sch, u64 expire, u64 delta_ns) __ksym;
void bpf_qdisc_bstats_update(struct Qdisc *sch, const struct sk_buff *skb) __ksym;

static struct qdisc_skb_cb *qdisc_skb_cb(const struct sk_buff *skb)
{
	return (struct qdisc_skb_cb *)skb->cb;
}

static inline unsigned int qdisc_pkt_len(const struct sk_buff *skb)
{
	return qdisc_skb_cb(skb)->pkt_len;
}

#endif
