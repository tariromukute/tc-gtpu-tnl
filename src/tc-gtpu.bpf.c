#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/pkt_cls.h>
#include <linux/swab.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include "logging.h"
#include "tc-gtpu.h"
#include "parsing_helpers.h"

#define TC_ACT_UNSPEC         (-1)
#define TC_ACT_OK               0
#define TC_ACT_SHOT             2
#define TC_ACT_STOLEN           4
#define TC_ACT_REDIRECT         7

#define ETH_P_IP 0x0800 /* Internet Protocol packet */
#define __section(x) __attribute__((section(x), used))

#define DEFAULT_QFI 9

#define SAMPLE_SIZE 1024ul
#define MAX_CPUS 128
#define MAX_TCP_SIZE 1448

#define min(x, y) ((x) < (y) ? (x) : (y))

/* Metadata will be in the perf event before the packet data. */
struct S {
	__u16 cookie;
	__u16 pkt_len;
} __attribute__((__packed__));;

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, __u32); // teid
	__type(value, struct ingress_state);
	__uint(max_entries, 32);
    // __uint(pinning, LIBBPF_PIN_BY_NAME);
} ingress_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, __u32); // ifindex
	__type(value, struct egress_state);
	__uint(max_entries, 32);
    // __uint(pinning, LIBBPF_PIN_BY_NAME);
} egress_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
	__type(key, int);
	__type(value, __u32);
	__uint(max_entries, MAX_CPUS);
} pcap_map SEC(".maps");

const volatile struct gtpu_config config;

static struct ipv4_gtpu_encap ipv4_gtpu_encap = {
	.ipv4h.version = 4,
    .ipv4h.ihl = 5,
    .ipv4h.ttl = 64,
    .ipv4h.protocol = IPPROTO_UDP,
    .ipv4h.saddr = bpf_htonl(0x0a000304), // 10.0.3.4
    .ipv4h.daddr = bpf_htonl(0x0a000305), // 10.0.3.5
    .ipv4h.check = 0,

    .udp.source = bpf_htons(GTP_UDP_PORT),
	.udp.dest = bpf_htons(GTP_UDP_PORT),
    .udp.check = 0,
	
    .gtpu.flags = 0x34,
	.gtpu.message_type = GTPU_G_PDU,
    .gtpu.message_length = 0,
    
	.gtpu_hdr_ext.sqn = 0,
	.gtpu_hdr_ext.npdu = 0,
	.gtpu_hdr_ext.next_ext = GTPU_EXT_TYPE_PDU_SESSION_CONTAINER,
	.pdu.length = 1,
	.pdu.pdu_type = PDU_SESSION_CONTAINER_PDU_TYPE_UL_PSU,
	.pdu.next_ext = 0,
};

static struct ipv6_gtpu_encap ipv6_gtpu_encap = {
	.udp.source = bpf_htons(GTP_UDP_PORT),
	.udp.dest = bpf_htons(GTP_UDP_PORT),
	
    .gtpu.flags = 0x34,
	.gtpu.message_type = GTPU_G_PDU,
    .gtpu.message_length = 0,

	.gtpu_hdr_ext.sqn = 0,
	.gtpu_hdr_ext.npdu = 0,
	.gtpu_hdr_ext.next_ext = GTPU_EXT_TYPE_PDU_SESSION_CONTAINER,
	.pdu.length = 1,
	.pdu.pdu_type = PDU_SESSION_CONTAINER_PDU_TYPE_UL_PSU,
	.pdu.next_ext = 0,
};

/* Logic for checksum, thanks to https://github.com/facebookincubator/katran/blob/main/katran/lib/bpf/csum_helpers.h */
static __always_inline __u16 csum_fold_helper(__u32 csum)
{
	__u32 sum;
	sum = (csum >> 16) + (csum & 0xffff);
	sum += (sum >> 16);
	return ~sum;
}

static __always_inline void ipv4_csum(void* data_start, int data_size, __u64* csum) {
    *csum = bpf_csum_diff(0, 0, data_start, data_size, *csum);
    *csum = csum_fold_helper(*csum);
}

static __always_inline void ipv4_csum_inline(
    void* iph,
    __u64* csum) {
  __u16* next_iph_u16 = (__u16*)iph;
#pragma clang loop unroll(full)
    for (int i = 0; i < sizeof(struct iphdr) >> 1; i++) {
        *csum += *next_iph_u16++;
    }
    *csum = csum_fold_helper(*csum);
}


static __always_inline __u16
csum_fold_helperx(__u64 csum)
{
    int i;
#pragma unroll
    for (i = 0; i < 4; i++)
    {
        if (csum >> 16)
            csum = (csum & 0xffff) + (csum >> 16);
    }
    return ~csum;
}

static __always_inline __u16
iph_csum(struct iphdr *iph)
{
    iph->check = 0;
    unsigned long long csum = bpf_csum_diff(0, 0, (unsigned int *)iph, sizeof(struct iphdr), 0);
    return csum_fold_helperx(csum);
}

/* All credit goes to FedeParola from https://github.com/iovisor/bcc/issues/2463 */
__attribute__((__always_inline__))
static inline __u16 caltcpcsum(struct iphdr *iph, struct tcphdr *tcph, void *data_end)
{
    __u32 csum_buffer = 0;
    __u16 volatile *buf = (void *)tcph;

    // Compute pseudo-header checksum
    csum_buffer += (__u16)iph->saddr;
    csum_buffer += (__u16)(iph->saddr >> 16);
    csum_buffer += (__u16)iph->daddr;
    csum_buffer += (__u16)(iph->daddr >> 16);
    csum_buffer += (__u16)iph->protocol << 8;
    csum_buffer += bpf_htons(bpf_ntohs(iph->tot_len) - (__u16)(iph->ihl<<2));

    // Compute checksum on tcp header + payload
    for (int i = 0; i < MAX_TCP_SIZE; i += 2) 
    {
        if ((void *)(buf + 1) > data_end) 
        {
            break;
        }

        csum_buffer += *buf;
        buf++;
    }

    if ((void *)buf + 1 <= data_end) 
    {
        // In case payload is not 2 bytes aligned
        csum_buffer += *(__u8 *)buf;
    }

    __u16 csum = (__u16)csum_buffer + (__u16)(csum_buffer >> 16);
    csum = ~csum;

    return csum;
}

int handle_perf_pcap(struct __sk_buff *skb) {
    void *data_end = (void *)(unsigned long long)skb->data_end;
	void *data = (void *)(unsigned long long)skb->data;
    
    __u64 flags = BPF_F_CURRENT_CPU;
    __u16 sample_size = (__u16)(data_end - data);
    int ret;
    struct S metadata;

    metadata.cookie = 0xdead;
    metadata.pkt_len = min(sample_size, SAMPLE_SIZE);

    flags |= (__u64)sample_size << 32;

    ret = bpf_perf_event_output(skb, &pcap_map, flags,
                    &metadata, sizeof(metadata));

    if (ret) {
	    bpf_printk("perf_event_output failed: %d\n", ret);
    }

    return ret;
}

SEC("tc/egress")
int tnl_if_egress_fn(struct __sk_buff *skb)
{
    /**
     * The function is attached to the ingress of the tunnel interface associated with a UE.
     * The receives the data after it have been decapsulated at teh interface that receives
     * the GTPU packets (gtpu ingress). This functions does nothing to the data except for other
     * util functions like recording the number of received packets etc. It returns TC_ACT_OK
    */
    // bpf_printk("Received packet on tnl_if_ingress\n");
	void *data_end = (void *)(unsigned long long)skb->data_end;
	void *data = (void *)(unsigned long long)skb->data;
	struct ethhdr *eth = data;

	if (data + sizeof(struct ethhdr) > data_end)
		return TC_ACT_SHOT;

    if (config.verbose_level == LOG_VERBOSE)
        handle_perf_pcap(skb);
    
	if (eth->h_proto == ___constant_swab16(ETH_P_IP)) {
		return TC_ACT_OK;
    } else {
		return TC_ACT_OK;
    }
};

SEC("tc/ingress")
int tnl_if_ingress_fn(struct __sk_buff *skb)
{
    /**
     * This function is attached to the egress of the tunnel interface associated with a UE.
     * The function encapsulates the IP data with a GTPU header. If the tnl_interfaces_map
     * contains data for the index of the interface, it will encapsulate the IP data based on
     * the values from the map i.e., qfi, tied etc. If the map doesn't have values, the qfi 
     * will equal the default qfi defined (DEFAULT_QFI) and tied will equal the interface 
     * index. The function then redirects the output to the egress of the gtpu_interface 
     * interface (the global const volatile variable gtpu_interface). It sets the destination
     * ip to the gtpu_dest_ip (global const volatile variable gtpu_dest_ip).
    */
    // bpf_printk("Received packet on tnl_if_egress\n");
    void *data_end = (void *)(unsigned long long)skb->data_end;
    void *data = (void *)(unsigned long long)skb->data;
    int eth_type, ip_type;
    struct hdr_cursor nh = { .pos = data };
    struct ethhdr *eth = data;
    struct iphdr *iphdr;
    struct tcphdr *tcphdr;
    __u64 csum = 0;
    __u32 qfi, teid;
    __u32 key = skb->ifindex;
    struct egress_state *state;
    __u64 flags;

    int payload_len = (data_end - data) - sizeof(struct ethhdr);
    
    if (config.verbose_level == LOG_VERBOSE)
        handle_perf_pcap(skb);

    eth_type = parse_ethhdr(&nh, data_end, &eth);
	if (eth_type != bpf_htons(ETH_P_IP))
        goto out;

    ip_type = parse_iphdr(&nh, data_end, &iphdr);
	if (ip_type < 0)
		goto out;

    // check if skb is non-linear, it if is and pull in non-linear data
    if (bpf_ntohs(iphdr->tot_len) > payload_len)
        if (bpf_skb_pull_data(skb, bpf_ntohs(iphdr->tot_len) + sizeof(struct ethhdr)) < 0)
            return TC_ACT_UNSPEC;

    data_end = (void *)(unsigned long long)skb->data_end;
    data = (void *)(unsigned long long)skb->data;
    nh.pos = data;
    payload_len = (data_end - data) - sizeof(struct ethhdr);

    // Logic to fetch QFI and TEID from maps or use defaults
    state = bpf_map_lookup_elem(&egress_map, &key);
    if (state && state->teid && state->qfi) {
        qfi = state->qfi;
        teid = state->teid;
    } else {
        qfi = DEFAULT_QFI;
        teid = skb->ifindex;  // Use interface index as default TEID
    }
           
    flags = // BPF_F_ADJ_ROOM_FIXED_GSO |
          BPF_F_ADJ_ROOM_ENCAP_L3_IPV4 |
          BPF_F_ADJ_ROOM_ENCAP_L4_UDP;

    int roomlen = sizeof(struct ipv4_gtpu_encap);
    int ret = bpf_skb_adjust_room(skb, roomlen, BPF_ADJ_ROOM_MAC, flags);
    if (ret) {
        bpf_printk("error calling skb adjust room %d, error code %d\n", roomlen, ret);
        return TC_ACT_SHOT;
    }

    // Adjust pointers to new packet location after possible linearization
    data_end = (void *)(unsigned long long)skb->data_end;
    data = (void *)(unsigned long long)skb->data;
    eth = data;

    ipv4_gtpu_encap.ipv4h.daddr = config.daddr.addr.addr4.s_addr;
    ipv4_gtpu_encap.ipv4h.saddr = config.saddr.addr.addr4.s_addr;
    ipv4_gtpu_encap.ipv4h.tot_len = bpf_htons(sizeof(struct ipv4_gtpu_encap) + payload_len);

    // For checksum to be recalculated
    bpf_set_hash_invalid(skb);
    // ipv4_gtpu_encap.ipv4h.check = csum_fold_helper(bpf_csum_diff((__be32 *)&ipv4_gtpu_encap.ipv4h, 0, (__be32 *)&ipv4_gtpu_encap.ipv4h, sizeof(struct iphdr), 0));
    
    ipv4_gtpu_encap.udp.len = bpf_htons(sizeof(struct ipv4_gtpu_encap) + payload_len - sizeof(struct iphdr));

    ipv4_gtpu_encap.gtpu.teid = bpf_htonl(teid);
    ipv4_gtpu_encap.gtpu.message_length = bpf_htons(payload_len + sizeof(struct gtpu_hdr_ext) + sizeof(struct gtp_pdu_session_container));

    int offset = sizeof(struct ethhdr);
    ret = bpf_skb_store_bytes(skb, offset, &ipv4_gtpu_encap, roomlen, 0);
    if (ret) {
        bpf_printk("error storing ip header\n");
        return TC_ACT_SHOT;
    }
    
    // bpf_printk("Redirecting to gtpu interface\n");
    return bpf_redirect_neigh(config.gtpu_ifindex, NULL, 0, 0);

out:
    return TC_ACT_OK;
}


SEC("tc/ingress")
int gtpu_ingress_fn(struct __sk_buff *skb)
{
    /**
     * The function is attched to the ingress of the interface attached to the external
     * network, the gtpu_interface. The function decapsulates the GTPU header of the 
     * incoming packets and sends it to the ingress of the tunnel interface (tnl_interface).
     * The function gets the tunnel interface by checking the tied_map to get interface to
     * send to based on the tied of the incoming GTPU packet. If the tied_map does not contain
     * a valid value, it will treat the tied as the interface index to send to.
    */
    // bpf_printk("Received packet on gtpu_ingress\n");
	void *data_end = (void *)(unsigned long long)skb->data_end;
	void *data = (void *)(unsigned long long)skb->data;
    int eth_type, ip_type, err;
	struct hdr_cursor nh = { .pos = data };
	struct ethhdr *eth;
	struct iphdr *iphdr;
    struct udphdr *udphdr;
    struct gtpuhdr *gtpuhdr;
	struct gtp_pdu_session_container *pdu;
    int tnl_interface;
    __u32 key, qfi;
    struct ingress_state *state;

    // Check if the incoming packet is GTPU
	eth_type = parse_ethhdr(&nh, data_end, &eth);
	if (eth_type != bpf_htons(ETH_P_IP))
		goto out;

    if (config.verbose_level == LOG_VERBOSE)
        handle_perf_pcap(skb);

	ip_type = parse_iphdr(&nh, data_end, &iphdr);
	if (ip_type != IPPROTO_UDP)
		goto out;

	if (parse_udphdr(&nh, data_end, &udphdr) < 0)
		goto out;

    if (udphdr->dest != bpf_htons(GTP_UDP_PORT))
        goto out;

    if (parse_gtpuhdr(&nh, data_end, &gtpuhdr) < 0)
		goto out;
    
    if (gtpuhdr->message_type == 0x1a) // Error indication
        return TC_ACT_SHOT;

    key = bpf_ntohl(gtpuhdr->teid);
    state = bpf_map_lookup_elem(&ingress_map, &key);
    if (state && state->qfi && state->ifindex) {
        qfi = state->qfi;
        tnl_interface = state->ifindex;
    } else {
        qfi = DEFAULT_QFI;
        tnl_interface = gtpuhdr->teid; // default ifindex = teid
    }

    int roomlen = sizeof(struct ipv4_gtpu_encap);
    int ret = bpf_skb_adjust_room(skb, -roomlen, BPF_ADJ_ROOM_MAC, 0);
    if (ret) {
        bpf_printk("error reducing skb adjust room.\n");
        return TC_ACT_SHOT;
    }

    // Adjust pointers to new packet location after possible linearization
    data_end = (void *)(unsigned long long)skb->data_end;
    data = (void *)(unsigned long long)skb->data;
    eth = data;

    nh.pos = data;

    eth_type = parse_ethhdr(&nh, data_end, &eth);
	if (eth_type != bpf_htons(ETH_P_IP))
		goto out;

    if (state && state->qfi && state->ifindex) {
        __builtin_memcpy(eth->h_dest, state->if_mac, ETH_ALEN);
    }

out:
    return TC_ACT_OK;
};

SEC("tc/egress")
int gtpu_egress_fn(struct __sk_buff *skb)
{
     /**
     * The function is attched to the egress of the interface attached to the external
     * network. It receives GTPU encapuslated packets from the tunnel interfaces. This 
     * functions does nothing to the packet data except for other util functions like 
     * recording the number of received packets etc. It the sends the packet out.
    */
    // bpf_printk("Received packet on gtpu_egress\n");
	void *data_end = (void *)(unsigned long long)skb->data_end;
	void *data = (void *)(unsigned long long)skb->data;
    int eth_type, ip_type, err;
    struct hdr_cursor nh = { .pos = data };
	struct ethhdr *eth;
    struct gtpuhdr *ghdr;
	struct gtp_pdu_session_container *pdu;
	struct iphdr *iphdr;
    struct udphdr *udphdr;
    __u64 csum = 0;

    eth_type = parse_ethhdr(&nh, data_end, &eth);
	if (eth_type != bpf_htons(ETH_P_IP))
		goto out;

    if (config.verbose_level == LOG_VERBOSE)
        handle_perf_pcap(skb);

	ip_type = parse_iphdr(&nh, data_end, &iphdr);
	if (ip_type != IPPROTO_UDP)
		goto out;

	if (parse_udphdr(&nh, data_end, &udphdr) < 0)
		goto out;

    if (udphdr->dest != bpf_htons(GTP_UDP_PORT))
        goto out;

    ipv4_csum_inline(iphdr, &csum);
    iphdr->check = csum;

out:
    return TC_ACT_OK;
};

char __license[] __section("license") = "GPL";

// // Calculate the checksum
//     __u32 gtpu_len = sizeof(struct gtpuhdr) + sizeof(struct gtpu_hdr_ext) + sizeof(struct gtp_pdu_session_container);
//     if (nh.pos + gtpu_len > data_end)
//         goto out;

//     nh.pos += gtpu_len;

//     ip_type = parse_iphdr(&nh, data_end, &iphdr);
// 	if (ip_type < 0) {
//         bpf_printk("ip_type < 0");
// 		goto out;
//     }


//     // // ipv4_csum_inline(iphdr, &csum);
//     iphdr->check = 0;
//     iphdr->check = iph_csum(iphdr);
//     bpf_printk("Checksum %lx", iphdr->check);

//     if (ip_type == IPPROTO_TCP) { // TCP
        
//         if (parse_tcphdr(&nh, data_end, &tcphdr) < 0) {
//              bpf_printk("parse_tcphdr(&nh, data_end, &tcphdr)");
// 			goto out;
// 		}
        
//         // if ((void *)tcphdr + 1 > data_end)
//         //     goto out;
//         tcphdr->check = 0;
//         // tcphdr->check = caltcpcsum(iphdr, tcphdr, data_end);
//         // bpf_printk("TCP Checksum %lx", tcphdr->check);
//     }