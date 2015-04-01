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

#define BUF_SIZE 500

/* Questions
		1. how much buffering are we allowed?
*/


struct reliable_state
{
	rel_t *next;            /* Linked list for traversing all connections */
	rel_t **prev;

	conn_t *c;          /* This is the connection object */

	int effective_window;
	int advertised_window;

	char* send_buffer;
	char* last_byte_acked;
	char* last_byte_sent;
	char* last_byte_written;
	int send_buf_space;
	int max_send_buffer;

	char* rcv_buffer;
	char* last_byte_read;
	char* next_byte_expected;
	char* last_byte_received;
	int rcv_buf_space;
	int max_rcv_buffer;
};


rel_t *rel_list;
int cur_effective_window;


/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t* rel_create (conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc)
{
	/* allocate and zero memory for reliable struct */
	rel_t *r = xmalloc(sizeof(*r));
	memset (r, 0, sizeof(*r));

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

	/* set initial effective window */
	r->effective_window = BUF_SIZE;

	/* initialize send side */
	r->send_buf_space = BUF_SIZE;
	r->send_buffer = xmalloc(BUF_SIZE);
	r->last_byte_acked = r->send_buffer;
	r->last_byte_sent = r->send_buffer;
	r->last_byte_written = r->send_buffer;

	/* initialize receive side */
	r->rcv_buf_space = BUF_SIZE;
	r->rcv_buffer = xmalloc(BUF_SIZE);
	r->last_byte_read = r->rcv_buffer;
	r->next_byte_expected = r->rcv_buffer;
	r->last_byte_received = r->rcv_buffer;

	return r;
}


void rel_destroy (rel_t *r)
{
	/* reassigned linked list pointers */
	if (r->next)
		r->next->prev = r->prev;
	*r->prev = r->next;

	/* free connection struct */
	conn_destroy(r->c);

	/* free send and receive buffers */
	free(r->send_buffer);
	free(r->rcv_buffer);

	/* free connection struct */
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
	r->last_byte_acked = pkt->ackno - 1; //TODO

	/* process data packet */
	if(pkt->len >= 12) {
		/* copy payload to receive buffer */
		int data_len = pkt->len - 12;
		memcpy(pkt->data, r->next_byte_expected, data_len);

		/* update receive state */
		r->rcv_buf_space -= data_len; //TODO all this shit is wrong
		r->last_byte_received += data_len;
		r->next_byte_expected += data_len;

		/* update effective window */
		r->effective_window = pkt->advertised_window - (r->last_byte_sent - r->last_byte_acked);

		/* update and send advertise window */
		r->advertised_window = r->max_rcv_buffer - ((r->next_byte_expected - 1) - r->last_byte_read);
	}

}


void rel_read (rel_t *s)
{
	int rd_len;

	while (s->send_buf_space > 0) {
		rd_len = conn_input(s->c, s->last_byte_written + 1, s->send_buf_space);

		if (rd_len == 0) {
			return;
		} else if (rd_len > 0) {
			s->last_byte_written += rd_len;
			/* TODO send packet */
			// int send_len = conn_sendpkt(s->c, pkt, sizeof(pkt))
		} else {
			//TODO handle EOF
			return;
		}

	}
}

void rel_output (rel_t *r)
{
	size_t buf_space = conn_bufspace(r->c);

	if (buf_space == 0) {
		return;
	} else {

		while (r->last_byte_read < r->last_byte_received
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
