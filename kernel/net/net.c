// FIXME: assert maximum size in net_send_*
#include <assert.h>
#include <stdbool.h>

#include <console.h>
#include <heap.h>
#include <list.h>
#include <module.h>
#include <net/net.h>
#include <net/e1000.h>
#include <process.h>
#include <string.h>

// TODO: we need a global routing table, an option to enable ipv4 forwarding
// TODO: arp table for each network interface

static int ktask_net(void *extra, char *name);

list_t *netif_list;

/* big endian to little endian helpers */
// host to network long
uint32_t htonl(uint32_t hostlong) {
	return ((hostlong & 0xFF) << 24) | ((hostlong & 0xFF00) << 8) |
		((hostlong & 0xFF0000) >> 8) | ((hostlong & 0xFF000000) >> 24);
}
// host to network short
uint16_t htons(uint16_t hostshort) {
	return ((hostshort & 0x00FF) << 8) | ((hostshort & 0xFF00) >> 8);
}
// network to host long
uint32_t ntohl(uint32_t netlong) {
	return ntohl(netlong);
}

// network to host short
uint16_t ntohs(uint16_t netshort) {
	return htons(netshort);
}

/* checksum helpers */
static uint16_t calc_checksum(uint8_t *data, size_t size) {
	uint32_t v = 0;
	uint16_t *s = (uint16_t *)data;
	for (unsigned int i = 0; i < (size / sizeof(uint16_t)); i++) {
		v += ntohs(s[i]);
	}

	if (v > 0xFFFF) {
		v = ((v & 0xFFFF0000) >> 16) + (v & 0xFFFF);
	}

	uint16_t checksum = (~( v & 0xFFFF)) & 0xFFFF;
	return checksum;
}

static uint16_t ipv4_calculate_checksum(ipv4_packet_t *packet) {
	uint16_t old_checksum = packet->checksum;
	packet->checksum = 0;
	uint16_t checksum = calc_checksum((uint8_t *)packet, sizeof(ipv4_packet_t));
	packet->checksum = old_checksum;
	return checksum;
}

static bool ipv4_is_checksum_valid(ipv4_packet_t *packet) {
	uint16_t calc_checksum = ipv4_calculate_checksum(packet);
	return calc_checksum == ntohs(packet->checksum);
}

/**/
void net_register_netif(send_packet_t send, receive_packet_t receive, uint8_t *mac, void *extra) {
	assert(send != NULL);
	assert(receive != NULL);
	assert(extra != NULL); // technically valid
	netif_t *netif = kcalloc(1, sizeof(netif_t));
	assert(netif != NULL);
	netif->send_packet = send;
	netif->receive_packet = receive;
	netif->extra = extra;
	memcpy(&netif->mac[0], mac, 6);
	netif->ip[0] = 10;
	netif->ip[1] = 0;
	netif->ip[2] = 0;
	netif->ip[3] = 2;
	list_insert(netif_list, netif);

	create_ktask(ktask_net, "[net]", netif);
}

static bool net_send_ethernet(netif_t *netif, uint8_t *src, uint8_t *dest, enum ethernet_type ethernet_type, uint8_t *data, size_t data_size) {
	// TODO: handle routing here
	size_t size = sizeof(ethernet_packet_t) + data_size;
	if (size < 60) { // padding
		size = 60;
	}
	ethernet_packet_t *packet = kcalloc(1, size);
	if (packet == NULL) {
		printf("out of memory!\n");
		return false;
	}
	memcpy(packet->dest, dest, 6);
	memcpy(packet->src, src, 6);
	packet->type = htons(ethernet_type);
	memcpy(packet->data, data, data_size);
	netif->send_packet(netif->extra, (uint8_t *)packet, size);
	kfree(packet);
	return true;
}

static bool net_send_arp(netif_t *netif, uint8_t *srcmac, uint8_t *destmac, uint16_t opcode, uint8_t *srchw, uint8_t *srcpr, uint8_t *dsthw, uint8_t *dstpr) {
	size_t size = sizeof(arp_packet_t);
	arp_packet_t *packet = kcalloc(1, size);
	if (packet == NULL) {
		printf("out of memory!\n");
		return false;
	}
	packet->hwtype = htons(ARP_HWTYPE_ETHERNET);
	packet->ptype = htons(ETHERNET_TYPE_IPV4);
	packet->hlen = 6;
	packet->plen = 4;
	packet->opcode = htons(opcode);
	memcpy(packet->srchw, srchw, 6);
	memcpy(packet->srcpr, srcpr, 4);
	memcpy(packet->dsthw, dsthw, 6);
	memcpy(packet->dstpr, dstpr, 4);
	bool success = net_send_ethernet(netif, srcmac, destmac, ETHERNET_TYPE_ARP, (uint8_t *)packet, size);
	kfree(packet);
	return success;
}

static bool net_send_ipv4(netif_t *netif, uint8_t *srcmac, uint8_t *destmac, uint16_t identification, enum ipv4_type protocol, uint8_t *srcip, uint8_t *destip, uint8_t *data, size_t data_size) {
	size_t size = sizeof(ipv4_packet_t) + data_size;
	ipv4_packet_t *packet = kcalloc(1, size);
	if (packet == NULL) {
		printf("out of memory!\n");
		return false;
	}
	packet->version_ihl = 0x45; // version 5 length 20 bytes
	packet->dscp_ecn = 0; // unused
	packet->length = htons(size);
	packet->identification = htons(identification);
//	packet->flags_fragment_offset = htons(0x4000);
	packet->flags_fragment_offset = 0; // not implemented
	packet->ttl = 64;
	packet->protocol = protocol;
	memcpy(packet->srcip, srcip, 4);
	memcpy(packet->dstip, destip, 4);
	memcpy(packet->data, data, data_size);
	packet->checksum = htons(ipv4_calculate_checksum(packet));
	bool success = net_send_ethernet(netif, srcmac, destmac, ETHERNET_TYPE_IPV4, (uint8_t *)packet, size);
	kfree(packet);
	return success;
}

static void send_echo_reply(netif_t *netif, ethernet_packet_t *ethernet_packet, ipv4_packet_t *ipv4_packet, size_t ipv4_data_length, icmp_packet_t *icmp_packet) {
	assert(ipv4_data_length >= (sizeof(icmp_packet_t) + sizeof(icmp_echo_packet_t)));

	size_t size = ipv4_data_length;
	icmp_packet_t *icmp_reply = kcalloc(1, size);
	icmp_reply->type = ICMP_TYPE_ECHO_REPLY;
	icmp_reply->code = 0;
	icmp_reply->checksum = 0;
	icmp_echo_packet_t *echo_req = (icmp_echo_packet_t *)(&icmp_packet->data);
	icmp_echo_packet_t *echo_reply = (icmp_echo_packet_t *)(&icmp_reply->data);
	echo_reply->identifier = echo_req->identifier;
	echo_reply->sequence = echo_req->sequence;

	memcpy(echo_reply->data, echo_req->data, size - sizeof(ipv4_packet_t));
	icmp_reply->checksum = htons(calc_checksum((uint8_t *)icmp_reply, ipv4_data_length));

	bool success = net_send_ipv4(netif, netif->mac, ethernet_packet->src, ipv4_packet->identification,
		IPV4_TYPE_ICMP, netif->ip, ipv4_packet->srcip, (uint8_t *)icmp_reply, size);
	(void)success;
	kfree(icmp_reply);
}

static void handle_icmp(netif_t *netif, ethernet_packet_t *ethernet_packet, size_t ethernet_length,
	ipv4_packet_t *ipv4_packet, size_t ipv4_data_length) {
	(void)ethernet_length;
	if (ipv4_data_length < sizeof(icmp_packet_t)) {
		printf("    length too short!\n");
		return;
	}

	icmp_packet_t *icmp_packet = (icmp_packet_t *)(&ipv4_packet->data);
	printf("    type    : 0x%2x\n", icmp_packet->type);
	printf("    code    : 0x%2x\n", icmp_packet->code);
	printf("    checksum: 0x%4x\n", ntohs(icmp_packet->checksum));
	uint16_t old_checksum = ntohs(icmp_packet->checksum);
	icmp_packet->checksum = 0;
	if (old_checksum != calc_checksum((uint8_t *)icmp_packet, ipv4_data_length)) {
		printf("checksum invalid!\n");
		return;
	}
	icmp_packet->checksum = htons(old_checksum);

	switch(icmp_packet->type) {
		case ICMP_TYPE_ECHO_REQUEST:
			send_echo_reply(netif, ethernet_packet,
					ipv4_packet, ipv4_data_length,
					icmp_packet);
			printf("     icmp echo request\n");
			break;
		default:
			printf("     unknown type\n");
			break;
	}
}

static void handle_ipv4(netif_t *netif, ethernet_packet_t *ethernet_packet, size_t length) {
	if (length < (sizeof(ethernet_packet_t) + sizeof(ipv4_packet_t))) {
		printf("  length too short!\n");
		return;
	}

	ipv4_packet_t *ipv4_packet = (ipv4_packet_t *)(&ethernet_packet->data);
	if ((ipv4_packet->version_ihl & 0x0F) != 5) {
		printf("   ihl mismatch: 0x%x expected: 0x%x\n", ipv4_packet->version_ihl & 0xF, 5);
		return;
	}
	if (((ipv4_packet->version_ihl & 0xF0) >> 4) != 4) {
		printf("   version mismatch: 0x%x expected: 0x%x\n", (ipv4_packet->version_ihl & 0xF0) >> 4, 4);
	}
	// just ignore this (we need to implement quite a bit to support it)
	// printf("  dscp_ecn   : 0x%2x\n", ipv4_packet->dscp_ecn);
	size_t ipv4_packet_length = ntohs(ipv4_packet->length);
	printf("  length     : 0x%4x\n", ipv4_packet_length);
	if (ipv4_packet_length < 20) {
		printf("   packet too short!\n");
		return;
	}

	if (length < (ipv4_packet_length + sizeof(ethernet_packet_t) + 4)) {
		// 4 byte crc ethernet checksum
		printf("   ipv4 packet length invalid!\n");
		return;
	}
	printf("  identification: 0x%4x\n", ntohs(ipv4_packet->identification));
	uint16_t flags_fragment_offset = ntohs(ipv4_packet->flags_fragment_offset);
	uint8_t flags = (flags_fragment_offset & 0xE000) >> 13;
	// FIXME: convert into real bytes
	size_t fragment_offset = flags_fragment_offset & 0x1FFF;
	printf("  flags           : 0x%1x\n", flags);
	printf("  fragment offset : 0x%4x\n", fragment_offset);
	if (flags & 0x1) {
		printf("   reserved flag set!\n");
		return;
	}
	if (flags & 0x4) {
		// TODO: implement
		printf("   packet fragmented!\n");
		return;
	}
	printf("  flags fragment offset: 0x%4x\n", ntohs(ipv4_packet->flags_fragment_offset));
	printf("  ttl        : 0x%2x\n", ipv4_packet->ttl);
	// TODO: handle ttl=0 when routing
	printf("  protocol   : 0x%2x\n", ipv4_packet->protocol);

	printf("  checksum   : 0x%4x ", ntohs(ipv4_packet->checksum));
	if (! ipv4_is_checksum_valid(ipv4_packet)) {
		printf("invalid!\n");
		return;
	} else {
		printf("valid!\n");
	}
	printf("  src        : %u.%u.%u.%u\n",
		ipv4_packet->srcip[0], ipv4_packet->srcip[1], ipv4_packet->srcip[2], ipv4_packet->srcip[3]);
	printf("  dst        : %u.%u.%u.%u\n",
		ipv4_packet->dstip[0], ipv4_packet->dstip[1], ipv4_packet->dstip[2], ipv4_packet->dstip[3]);

	// TODO: handle extra headers

	if (!memcmp(ipv4_packet->dstip, netif->ip, 4)) {
		printf("   for us!\n");
		size_t data_length = ipv4_packet_length - 20;
		switch(ipv4_packet->protocol) {
			case IPV4_TYPE_ICMP:
				printf("   icmp\n");
				handle_icmp(netif, ethernet_packet, length, ipv4_packet, data_length);
				break;
			case IPV4_TYPE_TCP:
				printf("   tcp\n");
				break;
			case IPV4_TYPE_UDP:
				printf("   udp\n");
				break;
			default:
				printf("   unknown protocol!\n");
				break;
		}
	} else {
		// TODO: route the packet
		printf("   not for us!\n");
	}
	return;
}

// FIXME: not checking if arp and ethernet match
static void handle_arp(netif_t *netif, ethernet_packet_t *ethernet_packet, size_t length) {
	if (length < (sizeof(ethernet_packet_t) + sizeof(ipv4_packet_t))) {
		printf("  length too short!\n");
		return;
	}

	arp_packet_t *arp_packet = (arp_packet_t *)(&ethernet_packet->data);
	if (ntohs(arp_packet->hwtype) != 1) {
		printf("  hardware type mismatch: 0x%4x expected: 0x%4x\n", ntohs(arp_packet->hwtype), 1);
		return;
	}
	if (ntohs(arp_packet->ptype) != ETHERNET_TYPE_IPV4) {
		printf("  protocol mismatch: 0x%4x expected: 0x%4x\n", ntohs(arp_packet->ptype), ETHERNET_TYPE_IPV4);
		return;
	}
	if (arp_packet->hlen != 6) {
		printf("  hardware length mismatch: 0x%2x expected: 0x%2x\n", arp_packet->hlen, 6);
		return;
	}
	if (arp_packet->plen != 4) {
		printf("  protocol length mismatch: 0x%2x expected: 0x%2x\n", arp_packet->plen, 4);
	}

	switch (ntohs(arp_packet->opcode)) {
		case ARP_OPCODE_REQUEST:
			printf("  ARP REQUEST from %u.%u.%u.%u (%2x:%2x:%2x:%2x:%2x:%2x) for %u.%u.%u.%u (%2x:%2x:%2x:%2x:%2x:%2x)\n",
				arp_packet->srcpr[0], arp_packet->srcpr[1], arp_packet->srcpr[2], arp_packet->srcpr[3],
				arp_packet->srchw[0], arp_packet->srchw[1], arp_packet->srchw[2], arp_packet->srchw[3], arp_packet->srchw[4], arp_packet->srchw[5],
				arp_packet->dstpr[0], arp_packet->dstpr[1], arp_packet->dstpr[2], arp_packet->dstpr[3],
				arp_packet->dsthw[0], arp_packet->dsthw[1], arp_packet->dsthw[2], arp_packet->dsthw[3], arp_packet->dsthw[4], arp_packet->dsthw[5]);

			if (!memcmp(netif->ip, arp_packet->dstpr, 4)) {
				printf("   we are requesed, sending reply ...");
				if (net_send_arp(netif, netif->mac, ethernet_packet->src, ARP_OPCODE_REPLY,
					netif->mac, netif->ip, arp_packet->srchw, arp_packet->srcpr)) {
					printf(" sent!\n");
				} else {
					printf(" error!\n");
				}
			} else {
				printf("   not for us, ignoreing\n");
			}
			break;
		case ARP_OPCODE_REPLY:
			printf("  ARP REPLY from %u.%u.%u.%u is at %2x:%2x:%2x:%2x:%2x:%2x (for %u.%u.%u.%u (%2x:%2x:%2x:%2x:%2x:%2x))\n",
				arp_packet->srcpr[0], arp_packet->srcpr[1], arp_packet->srcpr[2], arp_packet->srcpr[3],
				arp_packet->srchw[0], arp_packet->srchw[1], arp_packet->srchw[2], arp_packet->srchw[3], arp_packet->srchw[4], arp_packet->srchw[5],
				arp_packet->dstpr[0], arp_packet->dstpr[1], arp_packet->dstpr[2], arp_packet->dstpr[3],
				arp_packet->dsthw[0], arp_packet->dsthw[1], arp_packet->dsthw[2], arp_packet->dsthw[3], arp_packet->dsthw[4], arp_packet->dsthw[5]);
			printf("   arp reply!\n");
			break;
		default:
			printf("unknown arp opcode!\n");
			printf("  opcode       : 0x%4x\n", ntohs(arp_packet->opcode));
			printf("  src hardware : %2x:%2x:%2x:%2x:%2x:%2x\n",
				arp_packet->srchw[0], arp_packet->srchw[1], arp_packet->srchw[2], arp_packet->srchw[3], arp_packet->srchw[4], arp_packet->srchw[5]);
			printf("  src ip       : %u.%u.%u.%u\n",
				arp_packet->srcpr[0], arp_packet->srcpr[1], arp_packet->srcpr[2], arp_packet->srcpr[3]);

			printf("  dst hardware : %2x:%2x:%2x:%2x:%2x:%2x\n",
				arp_packet->dsthw[0], arp_packet->dsthw[1], arp_packet->dsthw[2], arp_packet->dsthw[3], arp_packet->dsthw[4], arp_packet->dsthw[5]);
			printf("  dst ip       : %u.%u.%u.%u\n",
				arp_packet->dstpr[0], arp_packet->dstpr[1], arp_packet->dstpr[2], arp_packet->dstpr[3]);
			break;
	}
}

static int ktask_net(void *extra, char *name) {
	(void)name;
	netif_t *netif = (netif_t *)extra;
	if (netif == NULL) { // should not happen
		return -1;
	}

	while (1) {
		packet_t *recv_packet = netif->receive_packet(netif->extra);
		assert(recv_packet != NULL);
		size_t length = recv_packet->length;
		// just assume it's ethernet
		ethernet_packet_t *packet = (ethernet_packet_t *)(recv_packet->data);
		// we don't need it anymore, free it
		kfree(recv_packet);
		printf("received ethernet packet length: 0x%x\n", (uintptr_t)length);
		if (length < sizeof(ethernet_packet_t)) {
			printf("packet too short!\n");
			kfree(packet);
			continue;
		}

		// TODO: check checksum
		// TODO: check if the packet was meant for us, if not, route it

		printf(" dst: %2x:%2x:%2x:%2x:%2x:%2x\n",
			packet->dest[0], packet->dest[1], packet->dest[2], packet->dest[3], packet->dest[4], packet->dest[5]);
		printf(" src: %2x:%2x:%2x:%2x:%2x:%2x\n",
			packet->src[0], packet->src[1], packet->src[2], packet->src[3], packet->src[4], packet->src[5]);
		printf(" type: 0x%4x\n", ntohs(packet->type));
		switch (ntohs(packet->type)) {
			case ETHERNET_TYPE_ARP:
				handle_arp(netif, packet, length);
				break;
			case ETHERNET_TYPE_IPV4:
				printf(" ipv4\n");
				handle_ipv4(netif, packet, length);
				// TODO: implement
				break;
			default:
				printf("  unknown type\n");
		}
		kfree(packet);
	}
}

void net_init(void) {
	netif_list = list_init();
	assert(netif_list != NULL);
	e1000_init();
}

MODULE_INFO(net, net_init);