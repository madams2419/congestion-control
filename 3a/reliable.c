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
#define PKT_HDR_LEN 12
#define MAX_PACKET_SIZE 500
#define WAIT 1
#define NO_WAIT 0

#define RCV_BUF_SPACE(r) r->max_rcv_buffer - (r->last_pkt_received - r->last_pkt_read)
#define SEND_BUF_SPACE(r) r->max_send_buffer - (r->last_pkt_written - r->last_pkt_acked)

// Questions:

// TODO
// - figure out connection closing
// - check all requirements in 356 handout and Stanford handout


typedef struct packet_buf pbuf_t;

pbuf_t **create_srbuf(pbuf_t **srbuf, int len);
void destroy_srbuf(pbuf_t **srbuf, int len);
int get_buf_index(int sq_start, int sq_target, int buf_start, int buf_length);
int get_rbuf_index(int seqno, rel_t *r);
int get_sbuf_index(int seqno, rel_t *r);
pbuf_t *rbuf_from_seqno(int seqno, rel_t *r);
pbuf_t *sbuf_from_seqno(int seqno, rel_t *r);
void handle_connection_close(rel_t *r, int wait);
void send_packet(pbuf_t *pbuf, rel_t *s);
void send_next_packet(rel_t *s);
void send_ack(rel_t *s);
void hton_pconvert(packet_t *pkt);
void ntoh_pconvert(packet_t *pkt);
void per(char *st);
void per2(char *st, int i);
void ppkt(packet_t *pkt);
void print_buf_ptrs(rel_t *r);


struct reliable_state
{
	rel_t *next;            /* Linked list for traversing all connections */
	rel_t **prev;

	conn_t *c;          /* This is the connection object */

	int window;
	int timeout;
	int remote_eof_seqno;
	int local_eof_seqno;
	int fin_wait;

	pbuf_t **send_buffer;
	int max_send_buffer;
	int last_pkt_acked;
	int sbuf_start_index;
	int last_pkt_sent;
	int last_pkt_written;

	//TODO what should size of receive buffer be
	pbuf_t **rcv_buffer;
	int max_rcv_buffer;
	int num_dpkts_rcvd;
	int last_pkt_read;
	int rbuf_start_index;
	int next_pkt_expected;
	int last_pkt_received;
};

struct packet_buf
{
	int seqno;
	int data_len;
	char* data;
	struct timespec send_time;
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

	/* initialize config params */
	r->window = cc->window;
	r->timeout = cc->timeout;

	/* initialize booleans */
	r->remote_eof_seqno = 0;
	r->local_eof_seqno = 0;
	r->fin_wait = 0;

	/* initialize send side */
	r->max_send_buffer = r->window;
	r->send_buffer = create_srbuf(r->send_buffer, r->max_send_buffer);
	r->last_pkt_acked = 0;
	r->sbuf_start_index = 0;
	r->last_pkt_sent = 0;
	r->last_pkt_written = 0;

	/* initialize receive side */
	r->max_rcv_buffer = r->window; //TODO what should this be
	r->rcv_buffer = create_srbuf(r->rcv_buffer, r->max_rcv_buffer);
	r->num_dpkts_rcvd = 0;
	r->last_pkt_read = 0;
	r->rbuf_start_index = 0;
	r->next_pkt_expected = 0;
	r->last_pkt_received = 0;

	return r;
}


void rel_destroy(rel_t *r)
{
	/* reassigned linked list pointers */
	if (r->next)
		r->next->prev = r->prev;
	*r->prev = r->next;

	/* free connection struct */
	conn_destroy(r->c);

	/* free send buffer */
	destroy_srbuf(r->send_buffer, r->max_send_buffer);

	/* free receive buffers */
	destroy_srbuf(r->rcv_buffer, r->max_rcv_buffer);

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


void rel_recvpkt(rel_t *r, packet_t *pkt, size_t n)
{
	per("rel_recvpkt");
	uint16_t pkt_len = ntohs(pkt->len);

	/* verify packet length */
	if(pkt_len > n) {
		per("Packet length invalid!");
		fprintf(stderr, "pkt_len : %d", pkt_len);
		fprintf(stderr, "n       : %lu", n);
		return;
	}

	/* verify checksum */
	uint16_t cks = pkt->cksum;
	pkt->cksum = 0;
	if(cks != cksum(pkt, pkt_len)) {
		per("Checksum failed!");
		return;
	}

	/* convert packet to host byte order */
	ntoh_pconvert(pkt);

	/* update last byte acked regardless of packet type */
	if(pkt->ackno > 0 && pkt->ackno - 1 > r->last_pkt_acked) {
		r->last_pkt_acked = pkt->ackno - 1;
		r->sbuf_start_index = get_sbuf_index(r->last_pkt_acked + 1, r);
	}

	/* handle ack packet */
	if(pkt->len == ACK_LEN) {
		per2("Received ACK", pkt->ackno);
		handle_connection_close(r, NO_WAIT);
	}

	/* handle eof or data packet */
	else if(pkt->len >= PKT_HDR_LEN) {

		/* eof boolean */
		int isEOF = (pkt->len == PKT_HDR_LEN);

		/* debug printing */
		if(isEOF) {
			per("received EOF packet");
		} else {
			per("received DATA packet");
		}
		ppkt(pkt);

		/* return if packet is a duplicate */
		if(pkt->seqno < r->next_pkt_expected) {
			per("Packet is duplicate!");
			rel_output(r);
			return;
		}

		/* return if packet sequenced after eof */
		if(r->remote_eof_seqno > 0 && pkt->seqno >= r->remote_eof_seqno) {
			per("Packet sequenced after EOF");
			return;
		}

		/* handle data packet */
		if(!isEOF) {
			/* return if there is insufficient space in the buffer */
			int space_required = (r->last_pkt_received == -1) ? 1 : pkt->seqno - r->last_pkt_received;
			if(space_required > RCV_BUF_SPACE(r)) {
				print_buf_ptrs(r);
				per("Insufficient RCV buffer space.");
				return;
			}

			/* copy payload to receive buffer */
			int data_len = pkt->len - PKT_HDR_LEN;
			pbuf_t *rbuf = rbuf_from_seqno(pkt->seqno, r);
			rbuf->seqno = pkt->seqno;
			rbuf->data_len = data_len;
			memcpy(rbuf->data, pkt->data, data_len);

			/* handle first data packet */
			if(r->num_dpkts_rcvd == 0) {
				r->next_pkt_expected = pkt->seqno + 1;
				r->last_pkt_read = pkt->seqno - 1;
			}

			/* increment num data packets received */
			r->num_dpkts_rcvd++;

		}

		/* update last packet received */
		r->last_pkt_received = pkt->seqno;

		/* update next packet expected */
		if(pkt->seqno == r->next_pkt_expected) {
			r->next_pkt_expected++;
		}

		/* output packet */
		if(!isEOF) rel_output(r);

		/* additional EOF handling */
		if(isEOF) {
			r->remote_eof_seqno = pkt->seqno;
			handle_connection_close(r, WAIT);
		}

	}
}


void rel_read(rel_t *s)
{
	per("rel_read");

	//TODO protocol for determining how to structure packets
	//TODO convert send buffer from packet to byte granularity

	while (SEND_BUF_SPACE(s) > 0 && s->local_eof_seqno == 0) {

		/* get send buffer */
		pbuf_t *sbuf = sbuf_from_seqno(s->last_pkt_written + 1, s);
		int rd_len = conn_input(s->c, sbuf->data, MAX_PACKET_SIZE);
		int isEOF = (rd_len == -1);

		/* handle no data */
		if(rd_len == 0) {
			per("rel_read : no data");
			return;
		}

		/* set sent buffer seqno and length */
		sbuf->seqno = s->last_pkt_written + 1;
		sbuf->data_len = (isEOF) ? 0 : rd_len;

		/* update last packet written */
		s->last_pkt_written++;

		/* send next packet */
		send_next_packet(s);

		/* handle EOF */
		if(isEOF) {
			s->local_eof_seqno = sbuf->seqno;
			handle_connection_close(s, NO_WAIT);
		}

	}

	per("rel_read : end of while");
}


void rel_output(rel_t *r)
{
	per("rel_output");
	while (r->last_pkt_read < (r->next_pkt_expected - 1)) {
		size_t buf_space = conn_bufspace(r->c);
		pbuf_t *rbuf = rbuf_from_seqno(r->last_pkt_read + 1, r);

		/* return if there is insufficient space in the application buffer */
		if(buf_space < rbuf->data_len) {
			per("Insufficient space in conn_buffer.");
			return;
		}

		/* output packet */
		int out_len = conn_output(r->c, rbuf->data, rbuf->data_len);
		if(out_len <= 0) {
			per("Data output error.");
			return;
		}

		/* update last packet read seqno and buffer index */
		r->last_pkt_read++;
		r->rbuf_start_index = get_rbuf_index(r->last_pkt_read + 1, r);

		/* send ack */
		send_ack(r);

		/* check connection closed */
		handle_connection_close(r, NO_WAIT);
	}

}

void rel_timer ()
{
	// TODO loop through all connections
	rel_t *r = rel_list;
	struct timespec *tbuf = xmalloc(sizeof(struct timespec));
	int sn;
	for(sn = r->last_pkt_acked + 1; sn <= r->last_pkt_sent; sn++) {
		pbuf_t *sbuf = sbuf_from_seqno(sn, r);
		clock_gettime(CLOCK_MONOTONIC, tbuf);
		double t_elapsed_ms = 1000 * difftime(tbuf->tv_sec, sbuf->send_time.tv_sec);

		if(t_elapsed_ms >= r->timeout) {
			per2("retransmitting", sbuf->seqno);
			send_packet(sbuf, r);
		}

	}
	free(tbuf);
}


////////////////////////////////////////////////////
// HELPER FUNCTIONS
////////////////////////////////////////////////////


/* create send receive buffer */
pbuf_t **create_srbuf(pbuf_t **srbuf, int len) {
	srbuf = xmalloc(len * sizeof(*srbuf));
	int i;
	for(i = 0; i < len; i++) {
		srbuf[i] = xmalloc(sizeof(**srbuf));
		srbuf[i]->data = xmalloc(MAX_PACKET_SIZE);
	}
	return srbuf;
}


/* free send receive buffer memory */
void destroy_srbuf(pbuf_t **srbuf, int len) {
	int i;
	for(i = 0; i < len; i++) {
		free(srbuf[i]->data);
		free(srbuf[i]);
	}
}


/* get send or receive buffer index from sequence number target and start index */
int get_buf_index(int sq_start, int sq_target, int buf_start, int buf_length) {
	int offset = sq_target - sq_start;

	/* validate offset */
	if(offset < 0 || offset > buf_length) {
		per("Invalid offset.");
		return -1;
	}

	return (buf_start + offset) % buf_length;
}


/* get send buffer index from sequence number */
int get_sbuf_index(int seqno, rel_t *r) {
	return get_buf_index(r->last_pkt_acked + 1, seqno, r->sbuf_start_index, r->max_send_buffer);
}


/* get receive index from sequence number */
int get_rbuf_index(int seqno, rel_t *r) {
	return get_buf_index(r->last_pkt_read + 1, seqno, r->rbuf_start_index, r->max_rcv_buffer);
}


/* get send buffer from sequence number */
pbuf_t *sbuf_from_seqno(int seqno, rel_t *r) {
	return r->send_buffer[get_sbuf_index(seqno, r)];
}


/* get receive buffer from sequence number */
pbuf_t *rbuf_from_seqno(int seqno, rel_t *r) {
	return r->rcv_buffer[get_rbuf_index(seqno, r)];
}


/* checks if connection is closed and calls rel_destroy if so */
void handle_connection_close(rel_t *r, int wait) {
	if(r->local_eof_seqno > 0 &&							// local eof received
		r->remote_eof_seqno > 0 &&							// remote eof received
		r->last_pkt_acked == r->last_pkt_sent &&		// all packets sent have been acked
		r->last_pkt_read == r->remote_eof_seqno - 1)	// all packets up to eof have been outputted
	{
		/* wait two segment lifetimes if wait flag set */
		if(wait == WAIT) {
			r->fin_wait = 1;
		}

		rel_destroy(r);
	}
}


/* send single packet */
void send_packet(pbuf_t *pbuf, rel_t *s) {
	int pkt_len = pbuf->data_len + PKT_HDR_LEN;

	/* construct packet */
	packet_t *pkt = xmalloc(sizeof(packet_t));
	pkt->cksum = 0;
	pkt->len = pkt_len;
	pkt->ackno = s->next_pkt_expected;
	pkt->seqno = pbuf->seqno;
	memcpy(pkt->data, pbuf->data, pbuf->data_len);

	/* debug printing */
	if(pkt_len == 12) {
		per("sent EOF packet");
	} else {
		per("sent data packet");
	}
	ppkt(pkt);

	/* convert to network byte order */
	hton_pconvert(pkt);

	/* compute checksum */
	pkt->cksum = cksum(pkt, pkt_len);

	/* send packet */
	if(conn_sendpkt(s->c, pkt, pkt_len) > 0) {
		s->last_pkt_sent = (pbuf->seqno > s->last_pkt_sent) ? pbuf->seqno : s->last_pkt_sent;
		clock_gettime(CLOCK_MONOTONIC, &pbuf->send_time);
	} else {
		per("Packet sending failed!");
	}

	free(pkt);
}


/* send next packet in queue */
void send_next_packet(rel_t *s) {
	if(s->last_pkt_written == s->last_pkt_sent) {
		return;
	}

	/* retrieve buffer to send */
	pbuf_t *sbuf = sbuf_from_seqno(s->last_pkt_sent + 1, s);

	/* send packet */
	send_packet(sbuf, s);
}


/* send ack packet */
void send_ack(rel_t *s) {
	/* construct packet */
	packet_t *ack = xmalloc(ACK_LEN);
	ack->cksum = 0;
	ack->len = ACK_LEN;
	ack->ackno = s->next_pkt_expected;

	/* convert to network byte order */
	hton_pconvert(ack);

	/* compute checksum */
	ack->cksum = cksum(ack, ACK_LEN);

	/* send packet */
	if(conn_sendpkt(s->c, ack, ACK_LEN) < 0) {
		per2("Send failed for ACK", s->next_pkt_expected);
	} else {
		/* debug printing */
		per2("Sent ACK", s->next_pkt_expected);
	}

	free(ack);
}

/* convert packet fields from network to host byte order */
void ntoh_pconvert(packet_t *pkt) {
	pkt->len = ntohs(pkt->len);
	pkt->ackno = ntohl(pkt->ackno);
	if(pkt->len > ACK_LEN) {
		pkt->seqno = ntohl(pkt->seqno);
	}
}

/* convert packet fields from host to network byte order */
void hton_pconvert(packet_t *pkt) {
	if(pkt->len > ACK_LEN) {
		pkt->seqno = htonl(pkt->seqno);
	}
	pkt->len = htons(pkt->len);
	pkt->ackno = htonl(pkt->ackno);
}


/* print message with PID to standard error */
void per(char *st) {
	fprintf(stderr, "%d: %s\n", getpid(), st);
}
void per2(char *st, int i) {
	fprintf(stderr, "%d: %s %d\n", getpid(), st, i);
}


/* print packet data */
void ppkt(packet_t *pkt) {
	fprintf(stderr, "==========================\n");
	per("PACKET");
	fprintf(stderr, "==========================\n");
	fprintf(stderr, "length : %d\n", pkt->len);
	fprintf(stderr, "seqno  : %d\n", pkt->seqno);
	fprintf(stderr, "data   : %s\n", pkt->data);
	fprintf(stderr, "==========================\n");
}


/* print send and receive buffer pointers */
void print_buf_ptrs(rel_t *r) {
	fprintf(stderr, "==========================\n");
	fprintf(stderr, "SEND BUFFER\n");
	fprintf(stderr, "==========================\n");
	fprintf(stderr, "sbuf_start_index  : %d\n", r->sbuf_start_index);
	fprintf(stderr, "last_pkt_acked    : %d\n", r->last_pkt_acked);
	fprintf(stderr, "last_pkt_sent     : %d\n", r->last_pkt_sent);
	fprintf(stderr, "last_pkt_written  : %d\n", r->last_pkt_written);
	fprintf(stderr, "==========================\n");

	fprintf(stderr, "==========================\n");
	fprintf(stderr, "RECEIVE BUFFER\n");
	fprintf(stderr, "==========================\n");
	fprintf(stderr, "rbuf_start_index  : %d\n", r->rbuf_start_index);
	fprintf(stderr, "last_pkt_read     : %d\n", r->last_pkt_read);
	fprintf(stderr, "next_pkt_expected : %d\n", r->next_pkt_expected);
	fprintf(stderr, "last_pkt_received : %d\n", r->last_pkt_received);
	fprintf(stderr, "==========================\n");

	fprintf(stderr, "==========================\n");
	fprintf(stderr, "CLOSE STATE\n");
	fprintf(stderr, "==========================\n");
	fprintf(stderr, "remote_eof_seqno   : %d\n", r->remote_eof_seqno);
	fprintf(stderr, "local_eof_seqno    : %d\n", r->local_eof_seqno);
}
