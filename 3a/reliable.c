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

#define WINDOW_SIZE 1 // window size in number of packets (1 for stop and wait)

// Questions:

// TODO
// - check all requirements in 356 handout and Stanford handout



typedef struct packet_buf pbuf_t;

struct reliable_state
{
	rel_t *next;            /* Linked list for traversing all connections */
	rel_t **prev;

	conn_t *c;          /* This is the connection object */

	pbuf_t *send_buffer[WINDOW_SIZE];
	int last_pkt_acked;
	int last_pkt_sent;
	int last_pkt_written;

	pbuf_t *rcv_buffer[WINDOW_SIZE];
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

	/* initialize send side */
	memset(r->send_buffer, 0, sizeof(r->send_buffer));
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
	r->last_pkt_read = -1;
	r->next_pkt_expected = -1;
	r->last_pkt_received = -1;

	return r;
}


/* helper function to free send/rcv buffer memory */
void destroy_buf(pbuf_t** buf, int len) {
	for(int i = 0; i < len; i++) {
		free(buf[i]->data);
		free(buf[i]);
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

	/* free send buffers */
	for(int i = 0; i < sizeof(r->send_buffer); i++) {
		free(r->send_buffer[i]);
	}
	//TODO might have to free send_buffer here

	/* free receive buffers */
	destroy_buf(r->rcv_buffer, sizeof(r->rcv_buffer));

	/* free send buffer */
	destroy_buf(r->send_buffer, sizeof(r->send_buffer));

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

void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	/* update last byte acked regardless of packet type */
	r->last_pkt_acked = pkt->ackno - 1;

	/* process ack packet */
	if(pkt->len == 8) {


	}

	/* process data packet */
	else if(pkt->len >= 12) {

		//TODO verify length here

		/* copy payload to receive buffer */
		int data_len = pkt->len - 12;
		memcpy(pkt->data, r->rcv_buffer[pkt->seqno], data_len);

		/* update last packet received */
		r->last_pkt_received = pkt->seqno;

		/* update next packet expected */
		if(pkt->seqno == r->next_pkt_expected) {
			r->next_pkt_expected++;
		}

		/* attempt to output data */
		rel_output(r);

	}

}


void rel_read (rel_t *s)
{
	int rd_len;

	while (s->send_buf_space > 0) {
		rd_len = conn_input(s->c, s->last_pkt_written + 1, s->send_buf_space);

		if (rd_len == 0) {
			return;
		} else if (rd_len > 0) {
			s->last_pkt_written += rd_len;
			/* TODO send packet */
			// int send_len = conn_sendpkt(s->c, pkt, sizeof(pkt))
		} else {
			//TODO handle EOF
			return;
		}

	}
}

int rel_output (rel_t *r)
{
	size_t buf_space = conn_bufspace(r->c);

	if (buf_space == 0) {
		return 0;
	} else {

		while (r->last_pkt_read < r->next_pkt_expected
				&& buf_space > 0) {
			// print packets
		}

		if (buf_space > 0) {
			// update available buf space and send ack
		}
	}
}

void rel_timer ()
{
	/* Retransmit any packets that need to be retransmitted */

}
