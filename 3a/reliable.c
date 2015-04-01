#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

#define ACK_LEN 8
#define DATA_HDR_LEN 12
#define MAX_READ_LEN 500
#define WINDOW_SIZE 1 // window size in number of packets (1 for stop and wait)

#define RCV_BUF_SPACE(r) r->max_rcv_buffer - (r->last_pkt_received - r->last_pkt_read)
#define SEND_BUF_SPACE(r) r->max_send_buffer - (r->last_pkt_written - r->last_pkt_acked)

// Questions:
// - having seqno refer to packets instead of bytes really complicates things...do we have to do this? Is there anything internal to the library that demands it be packets?
// - do we have to conn_output partial packets or can we wait until there is space for an entier packet?
// - should we call rel_output when packets are recieved or let the program call it

// TODO
// - check all requirements in 356 handout and Stanford handout



typedef struct packet_buf pbuf_t;

struct reliable_state
{
	rel_t *next;            /* Linked list for traversing all connections */
	rel_t **prev;

	conn_t *c;          /* This is the connection object */

	int rcvd_remote_eof;
	int rcvd_local_eof;

	pbuf_t *send_buffer[WINDOW_SIZE];
	int max_send_buffer;
	int last_pkt_acked;
	int last_pkt_sent;
	int last_pkt_written;

	pbuf_t *rcv_buffer[WINDOW_SIZE];
	int max_rcv_buffer;
	int last_pkt_read;
	int next_pkt_expected;
	int last_pkt_received;
};

struct packet_buf
{
	int len;
	char* data;
};

rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t* rel_create (conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc)
{
	/* allocate and zero memory for reliable struct */
	rel_t *r = xmalloc(sizeof(*r));
	memset(r, 0, sizeof(*r));

	/* create connection struct if it does not exist */
	if (!c)
	{
		c = conn_create (r, ss);
		if (!c)
		{
			free (r);
			return NULL;
		}
	}

	/* add reliable struct to linked list */
	r->c = c;
	r->next = rel_list;
	r->prev = &rel_list;
	if (rel_list)
		rel_list->prev = &r->next;
	rel_list = r;

	/* initialize booleans */
	r->rcvd_remote_eof = 0;
	r->rcvd_local_eof = 0;

	/* initialize send side */
	memset(r->send_buffer, 0, sizeof(r->send_buffer));
	r->max_send_buffer = WINDOW_SIZE;
	r->last_pkt_acked = -1;
	r->last_pkt_sent = -1;
	r->last_pkt_written = -1;

	/* debug shit */
	printf("Send buffer size (from struct): %lu\n", sizeof(r->send_buffer)); //DEBUG
	printf("address of send_buffer      : %d\n", &r->send_buffer);
	printf("address of send_buffer[0]   : %d\n", &r->send_buffer[0]);
	printf("address of send_buffer[1]   : %d\n", &r->send_buffer[1]);
	printf("address of last_pkt_acked   : %d\n", &r->last_pkt_acked);

	/* initialize receive side */
	memset(r->rcv_buffer, 0, sizeof(r->rcv_buffer));
	r->max_rcv_buffer = WINDOW_SIZE;
	r->last_pkt_read = -1;
	r->next_pkt_expected = -1;
	r->last_pkt_received = -1;

	return r;
}


/* free packet buffer memory */
void destroy_pbuf(pbuf_t* pbuf) {
	free(pbuf->data);
	free(pbuf);
}


/* free send or receive buffer memory */
void destroy_buf(pbuf_t** buf, int len) {
	for(int i = 0; i < len; i++) {
		destroy_pbuf(buf[i]);
	}
}


void rel_destroy (rel_t *r)
{
	/* reassigned linked list pointers */
	if (r->next)
		r->next->prev = r->prev;
	*r->prev = r->next;

	/* free connection struct */
	conn_destroy(r->c);

	/* free send buffer */
	destroy_buf(r->send_buffer, sizeof(r->send_buffer));

	/* free receive buffers */
	destroy_buf(r->rcv_buffer, sizeof(r->rcv_buffer));

	/* free reliable protocol struct */
	free(r);
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void rel_demux (const struct config_common *cc, const struct sockaddr_storage *ss, packet_t *pkt, size_t len)
{
}


/* get receive buffer from sequence number */
pbuf_t *rbuf_from_seqno(int seqno, rel_t *r) {
	//TODO
	return NULL;
}

/* get send buffer from sequence number */
pbuf_t *sbuf_from_seqno(int seqno, rel_t *r) {
	//TODO
	return NULL;
}


/* checks if connection is closed and calls rel_destroy if so */
void handle_connection_close(rel_t *r) {
	if(r->rcvd_local_eof == 1
			&& r->rcvd_remote_eof  == 1
			&& r->last_pkt_acked == r->last_pkt_written) {
		rel_destroy(r);
	}
}


void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	/* update last byte acked regardless of packet type */
	r->last_pkt_acked = pkt->ackno - 1;

	/* handle ack packet */
	if(pkt->len == ACK_LEN) {
		//TODO handle ack
	}

	/* handle eof or data packet */
	else if(pkt->len >= DATA_HDR_LEN) {

		/* verify checksum */
		uint16_t cks = pkt->cksum;
		pkt->cksum = 0;
		if(cks != cksum(pkt, pkt->len)) {
			printf("Checksum failed!\n");
			return;
		}

		//TODO verify length

		/* return if packet is a duplicate */
		if(pkt->seqno < r->next_pkt_expected) {
			return;
		}

		/* handle EOF packet */
		if(pkt->len == DATA_HDR_LEN) {
			r->rcvd_remote_eof = 1;
			handle_connection_close(r);
			return;
		}

		/* return if there is insufficient space in the buffer */
		int space_required = pkt->seqno - r->last_pkt_received;
		if(space_required > RCV_BUF_SPACE(r)) {
			return;
		}

		/* copy payload to receive buffer */
		int data_len = pkt->len - DATA_HDR_LEN;
		pbuf_t *rbuf = rbuf_from_seqno(pkt->seqno, r);
		rbuf->len = data_len;
		rbuf->data = xmalloc(rbuf->len);
		memcpy(pkt->data, rbuf->data, rbuf->len);

		/* update last packet received */
		r->last_pkt_received = pkt->seqno;

		/* update next packet expected */
		if(pkt->seqno == r->next_pkt_expected) {
			r->next_pkt_expected++;
		}

	}

}

void send_next_packet(rel_t *s) {
	if(s->last_pkt_written == s->last_pkt_sent) {
		return;
	}

	/* retrieve buffer to send */
	pbuf_t *sbuf = sbuf_from_seqno(s->last_pkt_sent + 1, s);

	/* construct packet */
	printf("Size of packet_t: %lu\n", sizeof(packet_t)); //DEBUG
	packet_t *pkt = xmalloc(sizeof(packet_t));
	pkt->cksum = 0;
	pkt->len = sbuf->len + DATA_HDR_LEN;
	pkt->ackno = s->next_pkt_expected - 1;
	pkt->seqno = s->last_pkt_sent + 1;
	pkt->cksum = cksum(pkt, pkt->len);

	/* send packet */
	if(conn_sendpkt(s->c, pkt, pkt->len) > 0) {
		s->last_pkt_sent++;
	} else {
		printf("Packet sending failed!\n");
	}

}


void rel_read(rel_t *s)
{
	int rd_len;

	//TODO protocol for determining how to structure packets
	//TODO convert send buffer from packet to byte granularity

	while (SEND_BUF_SPACE(s) > 0) {

		/* read user data input send buffer */
		char* temp = xmalloc(MAX_READ_LEN);
		rd_len = conn_input(s->c, temp, MAX_READ_LEN);

		/* handle EOF */
		if(rd_len == -1) {
			s->rcvd_local_eof = 1;
			handle_connection_close(s);
		}

		/* handle data */
		else if (rd_len > 0) {

			/* copy data data into send buffer */
			pbuf_t *sbuf = sbuf_from_seqno(s->last_pkt_written + 1, s);
			sbuf->len = rd_len;
			sbuf->data = xmalloc(sbuf->len);
			memcpy(temp, sbuf->data, sbuf->len);

			/* update last packet written */
			s->last_pkt_written++;

			/* send next packet */
			send_next_packet(s);

		}

		free(temp);

	}
}

void rel_output (rel_t *r)
{
	while (r->last_pkt_read < (r->next_pkt_expected - 1)) {
		size_t buf_space = conn_bufspace(r->c);
		pbuf_t *rbuf = rbuf_from_seqno(r->last_pkt_read + 1, r);

		/* return if there is insufficient space in the application buffer */
		if(buf_space < rbuf->len) {
			return;
		}

		/* output packet */
		if(conn_output(r->c, rbuf->data, rbuf->len) <= 0) {
			return;
		}

		/* free buffer */
		destroy_pbuf(rbuf);

		/* update last packet read */
		r->last_pkt_read++;

		/* send ack */
		packet_t *ack = xmalloc(ACK_LEN);
		ack->cksum = 0;
		ack->len = ACK_LEN;
		ack->ackno = r->last_pkt_read;
		ack->cksum = cksum(ack, ack->len);
		conn_sendpkt(r->c, ack, ack->len);
	}

}

void rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */

}
