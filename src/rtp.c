/*
 * rtp.c - A Tiny RTP Receiver
 * Inspired by VLC RTP access module
 *
 * Copyright (c) 2013   A. Dilly
 *
 * AirCat is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * AirCat is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with AirCat.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#include "rtp.h"

#ifndef MAX_RTP_PACKET_SIZE
	#define MAX_RTP_PACKET_SIZE 1500
#endif

#define DEFAULT_CACHE_SIZE 200
#define DEFAULT_CACHE_LOST 20

/**
 * RTP Header structure
 */
struct rtp_packet {
	/* Packet buffer (header + data) */
	unsigned char buffer[MAX_RTP_PACKET_SIZE];
	size_t len;
	/* Next packet pointer */
	struct rtp_packet *next;
};

struct rtp_handle {
	/* UDP socket */
	int sock;
	int port;
	unsigned int timeout;
	/* RTP parameters */
	unsigned int cache_size;
	unsigned int cache_lost;
	unsigned char payload;
	uint32_t ssrc;
	/* RTP cache */
	struct rtp_packet *packets; // RTP packet queue
	uint16_t next_seq; // Sequence number of next packet to read
	uint16_t max_seq; // Biggest sequence number received
	uint16_t bad_seq; // Used to detect sequence jump
	unsigned char initialized; // Flag to detect first packet
	unsigned char pending; // Flag to notice packets are waiting in queue
};

int rtp_open(struct rtp_handle **handle, unsigned int port, unsigned int cache_size, unsigned int cache_lost, unsigned long ssrc, unsigned char payload, unsigned int timeout)
{
	struct rtp_handle *h;
	struct sockaddr_in addr;
	int opt;

	/* Set a default cache size and cache lost */
	if(cache_size == 0)
		cache_size = DEFAULT_CACHE_SIZE;
	if(cache_lost == 0)
		cache_lost = DEFAULT_CACHE_LOST;
	if(cache_lost > cache_size)
		cache_lost = cache_size;

	/* Allocate structure */
	*handle = malloc(sizeof(struct rtp_handle));
	if(*handle == NULL)
		return -1;
	h = *handle;

	/* Init variables */
	h->port = port;
	h->ssrc = ssrc;
	h->payload = payload;
	h->timeout = timeout;
	h->cache_size = cache_size;
	h->cache_lost = cache_lost;
	h->packets = NULL;
	h->initialized = 0;
	h->pending = 0;

	/* Open socket */
	if((h->sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return -1;

	/* Set low delay on UDP socket */
#ifdef SO_PRIORITY
	opt = 6;
	if (setsockopt(h->sock, SOL_SOCKET, SO_PRIORITY, (const void *) &opt, sizeof(opt)) < 0)
		fprintf(stderr, "Can't change socket priority!\n");
#endif

#if defined(IPTOS_LOWDELAY) && defined(IP_TOS) && (defined(SOL_IP) || defined(IPPROTO_IP))
	{
		opt = IPTOS_LOWDELAY;
#ifdef SOL_IP
		if (setsockopt(h->sock, SOL_IP, IP_TOS, (const void *) &opt, sizeof(opt)) < 0)
#else
		if (setsockopt(h->sock, IPPROTO_IP, IP_TOS, (const void *) &opt, sizeof(opt)) < 0)
#endif
			fprintf(stderr, "Can't change socket TOS!\n");
	}
#endif

	/* Force socket to bind */
	opt = 1;
	if(setsockopt(h->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
		return -1;

	/* Bind */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if(bind(h->sock, (struct sockaddr *) &addr, sizeof(addr)) != 0)
		return -1;

	return 0;
}

static uint16_t inline rtp_get_sequence(struct rtp_packet *p)
{
	return (uint16_t) (((p->buffer[2] << 8) & 0xFF00) | (p->buffer[3] & 0x00FF));
}

static uint8_t inline rtp_get_payload(struct rtp_packet *p)
{
	return (uint8_t) (p->buffer[1] & 0x7F);
}

static uint32_t inline rtp_get_timestamp(struct rtp_packet *p)
{
	uint32_t *u = (uint32_t*)p->buffer;
	return ntohl(u[1]);
}

static uint32_t inline rtp_get_ssrc(struct rtp_packet *p)
{
	uint32_t *u = (uint32_t*)p->buffer;
	return ntohl(u[2]);
}

static int rtp_recv(struct rtp_handle *h, struct rtp_packet *packet)
{
	socklen_t sockaddr_size;
	struct sockaddr src_addr;
	int size;

	if(h == NULL || packet == NULL)
		return -1;

	/* Fill packet with 0 */
	memset(packet, 0, sizeof(struct rtp_packet));

	/* Get message from socket */
	sockaddr_size = sizeof(src_addr);
	if((size = recvfrom(h->sock, packet->buffer, MAX_RTP_PACKET_SIZE, 0, &src_addr, &sockaddr_size)) <= 0)
	{
		fprintf(stderr, "recvfrom() error!\n");
		return size;
	}

	/* Verify packet size */
	if(size < 12)
	{
		fprintf(stderr, "RTP packet is too short!\n");
		return -1;
	}

	/* Verify protocol version (accept only version 2) */
	if((packet->buffer[0] >> 6) != 2)
	{
		fprintf(stderr, "Unsupported RTP protocol version!\n");
		return -1;
	}

	/* Verify payload */
	if(rtp_get_payload(packet) != h->payload)
	{
		fprintf(stderr, "Bad RTP payload!");
		return -1;
	}

	/* Remove packet padding */
	if(packet->buffer[0] & 0x20)
	{
		/* Get padded bytes number */
		uint8_t pads = packet->buffer[size-1];
		if(pads == 0 && (pads + 12) > size)
			return -1;
		size -= pads;
	}

	/* Set not padded size in packet */
	packet->len = size;

	return 0;
}

/* Drop and Free packet if not valid */
static int rtp_queue(struct rtp_handle *h, struct rtp_packet *packet)
{
	struct rtp_packet **root;
	struct rtp_packet *cur;
//	uint32_t timestamp;
	int16_t delta;
	uint16_t seq;

	if(h == NULL || packet == NULL)
		return -1;

	/* Get packet infos */
//	timestamp = rtp_get_timestamp(packet);
	seq = rtp_get_sequence(packet);

	/* First packet received */
	if(!h->initialized)
	{
		h->next_seq = seq;
		h->bad_seq = seq-1;

		/* Get SSRC if not specified */
		if(h->ssrc == 0)
			h->ssrc = rtp_get_ssrc(packet);

		h->initialized = 1;
//		printf("First packet is %d\n", seq);
	}

	/* Check SSRC */
	if(h->ssrc != rtp_get_ssrc(packet))
	{
		fprintf(stderr, "Bad source in RTP packet!\n");
		goto drop;
	}

	/* Check sequence number */
	delta = seq - h->next_seq;
	/*
	 * /!\ OVERFLOW
	 */
	if(delta > h->cache_size)
	{
		if(h->bad_seq+1 == seq)
		{
			fprintf(stderr, "RTP jump: flush the cache\n");
			rtp_flush(h, seq);
		}
		else
		{
			fprintf(stderr, "RTP packet is too high!\n");
			h->bad_seq = seq;
			goto drop;
		}
	}
	else if (delta >= 0)
		h->max_seq = seq;

	/* Add packet to queue */
	root = &h->packets;
	cur = *root;
	while(cur != NULL)
	{
		/* Calculate delta position in queue
		 * Note: no overflow handling is needed 
		 * since delta is a signed short.
		 */
		delta = seq - rtp_get_sequence(cur);

		/* Previous packet found */
		if(delta < 0)
			break;
		else if(delta == 0)
		{
			/* Duplicate packet: drop it */
			fprintf(stderr, "Duplicate RTP packet!\n");
			goto drop;
		}

		root = &cur->next;
		cur = *root;
	}
	packet->next = *root;
	*root = packet;

	/* Check next available packet in queue */
	if(rtp_get_sequence(h->packets) != h->next_seq)
	{
		/*
		 * /!\ OVERFLOW
		 */
		if(h->next_seq + h->cache_lost > h->max_seq)
			return -1; /* Wait for packet */
	}

	return 0;

drop:
	free(packet);
	return -1;
}

static int rtp_dequeue(struct rtp_handle *h, unsigned char *buffer, size_t len)
{
	struct rtp_packet *packet;
	size_t offset;
	int32_t delta;

	if(h == NULL)
		return -1;

	/* No packets available */
	if(h->packets == NULL)
		return 0;

	/* Get next valid packet in queue */
	while((packet = h->packets) != NULL)
	{
		/* Calculate delta in sequence */
		delta = rtp_get_sequence(packet) - h->next_seq;
		/*
		 * /!\ OVERFLOW
		 */
		if(delta < 0)
		{
			/* Late packet: strange but drop it and get next */
			fprintf(stderr, "Late RTP packet!\n");
			h->packets = packet->next;
			free(packet);
			continue;
		}
		else if(delta > 0)
		{
			/* Lost packet */
			fprintf(stderr, "Lost RTP packet!\n");
			h->next_seq++;
			h->pending = 1;
			return 0;
		}

		/* Dequeue next packet */
		break;
	}

	/* Get start position of data in packet */
	offset = 12 + ((packet->buffer[0] &  0x0F) * 4);

	/* Slip extension header */
	if(packet->buffer[0] & 0x10)
	{
		offset += 4;
		if(offset > packet->len)
			goto drop;

		offset += (uint16_t)(((packet->buffer[offset-2] << 8) & 0xFF00) | (packet->buffer[offset-1] & 0x00FF));
	}

	/* Copy packet to buffer */
	if(len > packet->len-offset)
		len = packet->len - offset;
	memcpy(buffer, &packet->buffer[offset], len);

	/* Remove packet from queue */
	h->packets = packet->next;
	free(packet);

	/* Increment expected next sequence number */
	h->next_seq++;

	/* Check if next packet needs to be dequeue at next rtp_read() call */
	if(h->packets != NULL && h->next_seq == rtp_get_sequence(h->packets))
		h->pending = 1;
	else
		h->pending = 0;

	return len;

drop:
	h->packets = packet->next;
	free(packet);
	return 0;
}

/*
 * This function is synchronised on RTP packets.
 * Returns:
 *  - > 0 = len of next packet,
 *  - 0 if next packet is lost,
 *  - < 0 if an error occurs.
 */
int rtp_read(struct rtp_handle *h, unsigned char *buffer, size_t len)
{
	struct rtp_packet *packet;
	struct pollfd pfd;
	int ret;

	if(h == NULL)
		return -1;

	/* Some packets are always pending */
	if(h->pending)
		goto dequeue;

	/* Prepare poll for receiver */
	pfd.fd = h->sock;
	pfd.events = POLLIN;

	/* Receive next valid packet */
	while(1)
	{
		/* Wait for next incoming packet */
		ret = poll(&pfd, 1, h->timeout);
		if(ret < 0)
		{
			/* A signal has been received */
			if(errno == EINTR)
			{
				/* FIXME: the signal is not verified */
				fprintf(stderr, "RTP poll interrupted by signal!\n");
				return 0;
			}
			fprintf(stderr, "RTP socket error!\n");
			return -1;
		}
		else if(ret == 0)
		{
			/* Timeout reached: force packet dequeue */
			printf("RTP timeout\n");
			break;
		}

		/* New packet received */
		if (pfd.revents)
		{
			/* Allocate a new packet */
			packet = malloc(sizeof(struct rtp_packet));
			if(packet == NULL)
			{
				fprintf(stderr, "RTP memory allocation failed!\n");
				return -1;
			}

			/* Receive the packet */
			if(rtp_recv(h, packet) < 0)
			{
				/* Not a valid packet */
				printf("Bad packet %d\n", rtp_get_sequence(packet));
				free(packet);
				continue;
			}

			/* Add the packet to queue */
			if(rtp_queue(h, packet) < 0)
			{
				/* The packet is not valid or is not next 
				 * packet to dequeue and return
				 */
//				printf("Not valid queued packet %d\n", rtp_get_sequence(packet));
				continue;
			}

			/* A packet is ready to be dequeued (it can be a lost
			 * packet!).
			 */
			break;
		}
	}

dequeue:
	/* Get next packet in queue */
	return rtp_dequeue(h, buffer, len);
}

void rtp_flush(struct rtp_handle *h, unsigned int seq)
{
	struct rtp_packet *packet;

	if(h == NULL)
		return;

	/* Free cache */
	while(h->packets != NULL)
	{
		packet = h->packets;
		h->packets = packet->next;
		free(packet);
	}

	/* Update expected sequence number */
	h->next_seq = seq;
	h->max_seq = seq;
}

int rtp_close(struct rtp_handle *h)
{
	struct rtp_packet *packet;

	if(h == NULL)
		return 0;

	/* Free cache */
	while(h->packets != NULL)
	{
		packet = h->packets;
		h->packets = packet->next;
		free(packet);
	}

	/* Close socket */
	if(h->sock > 0)
		close(h->sock);

	free(h);

	return 0;
}
