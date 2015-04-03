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
#define MAX_PACKET_SIZE 500
#define RCV_BUF_SIZE 10

#define RCV_BUF_SPACE(r) r->max_rcv_buffer - (r->last_pkt_received - r->last_pkt_read)
#define SEND_BUF_SPACE(r) r->max_send_buffer - (r->last_pkt_written - r->last_pkt_acked)

// Questions:

// TODO
// - send intiial sending seqno to 1 (instead of 0)
// - remove rel_output call in rel_recpkt and see if shit still works
// - check all requirements in 356 handout and Stanford handout
// - move helper declarations to header file


typedef struct packet_buf pbuf_t;

pbuf_t **create_srbuf(pbuf_t **srbuf, int len);
void destroy_srbuf(pbuf_t **srbuf, int len);
int get_buf_index(int sq_start, int sq_target, int buf_start, int buf_length);
int get_rbuf_index(int seqno, rel_t *r);
int get_sbuf_index(int seqno, rel_t *r);
pbuf_t *rbuf_from_seqno(int seqno, rel_t *r);
pbuf_t *sbuf_from_seqno(int seqno, rel_t *r);
void handle_connection_close(rel_t *r);
void send_packet(pbuf_t *pbuf, rel_t *s);
void send_next_packet(rel_t *s);
void send_ack(rel_t *s);
void print_buf_ptrs(rel_t *r);


struct reliable_state
{
	rel_t *next;            /* Linked list for traversing all connections */
	rel_t **prev;

	conn_t *c;          /* This is the connection object */

	int window;
	int timeout;
	int rcvd_remote_eof;
	int rcvd_local_eof;

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
	printf("rel_create\n");
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
	r->rcvd_remote_eof = 0;
	r->rcvd_local_eof = 0;

	/* initialize send side */
	r->max_send_buffer = r->window;
	r->send_buffer = create_srbuf(r->send_buffer, r->max_send_buffer);
	r->last_pkt_acked = 0;
	r->sbuf_start_index = 0;
	r->last_pkt_sent = 0;
	r->last_pkt_written = 0;

	/* initialize receive side */
	r->max_rcv_buffer = RCV_BUF_SIZE; //TODO what should this be
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
	printf("rel_destroy\n");
	/* reassigned linked list pointers */
	if (r->next)
		r->next->prev = r->prev;
	*r->prev = r->next;

	/* free connection struct */
	conn_destroy(r->c);

	/* free send buffer */
	destroy_srbuf(r->send_buffer, sizeof(r->send_buffer)); //TODO proper send buffer size

	/* free receive buffers */
	destroy_srbuf(r->rcv_buffer, sizeof(r->rcv_buffer));

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
	printf("rel_recvpkt\n");
	/* verify packet length */
	if(pkt->len < n) {
		return;
	}

	/* verify checksum */
	uint16_t cks = pkt->cksum;
	pkt->cksum = 0;
	if(cks != cksum(pkt, pkt->len)) {
		fprintf(stderr, "Checksum failed!\n");
		return;
	}

	/* update last byte acked regardless of packet type */
	if(pkt->ackno > 0 && pkt->ackno - 1 > r->last_pkt_acked) {
		r->last_pkt_acked = pkt->ackno - 1;
		r->sbuf_start_index = get_sbuf_index(r->last_pkt_acked + 1, r);
	}

	/* handle ack packet */
	if(pkt->len == ACK_LEN) {
		handle_connection_close(r);
		print_buf_ptrs(r);
	}

	/* handle eof or data packet */
	else if(pkt->len >= DATA_HDR_LEN) {

		/* return if packet is a duplicate */
		if(pkt->seqno < r->next_pkt_expected) {
			return;
		}

		/* handle EOF packet */
		if(pkt->len == DATA_HDR_LEN) {
			r->rcvd_remote_eof = 1;
			handle_connection_close(r);
			print_buf_ptrs(r); //DEBUG
		}

		/* handle data packet */
		else {

			/* return if there is insufficient space in the buffer */
			int space_required = (r->last_pkt_received == -1) ? 1 : pkt->seqno - r->last_pkt_received;
			if(space_required > RCV_BUF_SPACE(r)) {
				return;
			}

			/* copy payload to receive buffer */
			int data_len = pkt->len - DATA_HDR_LEN;
			pbuf_t *rbuf = rbuf_from_seqno(pkt->seqno, r);
			rbuf->seqno = pkt->seqno;
			rbuf->data_len = data_len;
			memcpy(pkt->data, rbuf->data, rbuf->data_len);

			/* update last packet received */
			r->last_pkt_received = pkt->seqno;

			/* update next packet expected */
			if(pkt->seqno == r->next_pkt_expected) {
				r->next_pkt_expected++;
			}

			/* handle case of first data packet */
			if(r->num_dpkts_rcvd == 0) {
				r->next_pkt_expected = pkt->seqno + 1;
				r->last_pkt_read = pkt->seqno - 1;
			}

			/* increment num data packets received */
			r->num_dpkts_rcvd++;

			print_buf_ptrs(r); //DEBUG

			/* output packet */
			rel_output(r);

			/* send ack */
			send_ack(r);

		}
	}
}


void rel_read(rel_t *s)
{
	printf("rel_read\n");
	int rd_len;

	//TODO protocol for determining how to structure packets
	//TODO convert send buffer from packet to byte granularity

	while (SEND_BUF_SPACE(s) > 0) {

		/* read user data input send buffer */
		char *temp = xmalloc(MAX_PACKET_SIZE);
		rd_len = conn_input(s->c, temp, MAX_PACKET_SIZE);

		/* handle EOF */
		if(rd_len == -1) {
			s->rcvd_local_eof = 1;
			handle_connection_close(s);
		}

		/* handle no data */
		else if(rd_len == 0) {
			return;
		}

		/* handle data */
		else {

			/* copy data data into send buffer */
			pbuf_t *sbuf = sbuf_from_seqno(s->last_pkt_written + 1, s);
			sbuf->seqno = s->last_pkt_written + 1;
			sbuf->data_len = rd_len;
			memcpy(temp, sbuf->data, rd_len);

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
	printf("rel_output\n");
	while (r->last_pkt_read < (r->next_pkt_expected - 1)) {
		size_t buf_space = conn_bufspace(r->c);
		pbuf_t *rbuf = rbuf_from_seqno(r->last_pkt_read + 1, r);

		/* return if there is insufficient space in the application buffer */
		if(buf_space < rbuf->data_len) {
			return;
		}

		/* output packet */
		if(conn_output(r->c, rbuf->data, rbuf->data_len) <= 0) {
			return;
		}

		/* update last packet read seqno and buffer index */
		r->last_pkt_read++;
		r->rbuf_start_index = get_rbuf_index(r->last_pkt_read + 1, r);

	}

}

void rel_timer ()
{
	// TODO loop through all connections
	rel_t *r = rel_list;
	struct timespec *tbuf = xmalloc(sizeof(struct timespec));
	int sn;
	for(sn = r->last_pkt_acked + 1; sn < r->last_pkt_sent; sn++) {
		pbuf_t *sbuf = sbuf_from_seqno(sn, r);
		clock_gettime(CLOCK_MONOTONIC, tbuf);
		double t_elapsed_ms = 1000 * difftime(sbuf->send_time.tv_sec, tbuf->tv_sec);

		if(t_elapsed_ms > r->timeout) {
			send_packet(sbuf, r);
		}

	}
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
		fprintf(stderr, "Invalid offset.\n");
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
	return r->send_buffer[get_rbuf_index(seqno, r)];
}


/* get receive buffer from sequence number */
pbuf_t *rbuf_from_seqno(int seqno, rel_t *r) {
	return r->rcv_buffer[get_rbuf_index(seqno, r)];
}


/* checks if connection is closed and calls rel_destroy if so */
void handle_connection_close(rel_t *r) {
	if(r->rcvd_local_eof == 1
			&& r->rcvd_remote_eof  == 1
			&& r->last_pkt_acked == r->last_pkt_written) {
		rel_destroy(r);
	}
}


/* send single packet */
void send_packet(pbuf_t *pbuf, rel_t *s) {
	/* construct packet */
	packet_t *pkt = xmalloc(sizeof(packet_t));
	pkt->cksum = 0;
	pkt->len = pbuf->data_len + DATA_HDR_LEN;
	pkt->ackno = s->next_pkt_expected;
	pkt->seqno = pbuf->seqno;
	memcpy(pbuf->data, pkt->data, pbuf->data_len);
	pkt->cksum = cksum(pkt, pkt->len);


	/* send packet */
	if(conn_sendpkt(s->c, pkt, pkt->len) > 0) {
		s->last_pkt_sent++;
		clock_gettime(CLOCK_MONOTONIC, &pbuf->send_time);
	} else {
		fprintf(stderr, "Packet sending failed!\n");
	}

	print_buf_ptrs(s); //DEBUG
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
	packet_t *ack = xmalloc(ACK_LEN);
	ack->cksum = 0;
	ack->len = ACK_LEN;
	ack->ackno = s->next_pkt_expected;
	ack->cksum = cksum(ack, ack->len);
	conn_sendpkt(s->c, ack, ack->len);
}

void print_buf_ptrs(rel_t *r) {
	fprintf(stderr, "==========================\n");
	fprintf(stderr, "SEND BUFFER\n");
	fprintf(stderr, "==========================\n");
	fprintf(stderr, "last_pkt_acked    : %d\n", r->last_pkt_acked);
	fprintf(stderr, "last_pkt_sent     : %d\n", r->last_pkt_sent);
	fprintf(stderr, "last_pkt_written  : %d\n", r->last_pkt_written);
	fprintf(stderr, "==========================\n");

	fprintf(stderr, "==========================\n");
	fprintf(stderr, "RECEIVE BUFFER\n");
	fprintf(stderr, "==========================\n");
	fprintf(stderr, "last_pkt_read     : %d\n", r->last_pkt_read);
	fprintf(stderr, "next_pkt_expected : %d\n", r->next_pkt_expected);
	fprintf(stderr, "last_pkt_received : %d\n", r->last_pkt_received);
	fprintf(stderr, "==========================\n");
}
