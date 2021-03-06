/*
 * Copyright (c) 2014, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

/**
 * \file
 *         TSCH.
 * \author
 *         Beshr Al Nahas <beshr@sics.se>
 */
#include "contiki.h"
#include "contiki-conf.h"
#include "tsch.h"
#include "net/packetbuf.h"
#include "net/queuebuf.h"
#include "net/netstack.h"
#include "net/rime/rimestats.h"
#include <string.h>
#include "sys/rtimer.h"
#include "cooja-debug.h"
#include "lib/list.h"
#include "lib/memb.h"
#include "lib/random.h"
#include "dev/cc2420-tsch.h"

static volatile ieee154e_vars_t ieee154e_vars;

#define DEBUG 0
#if DEBUG
#include <stdio.h>
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif

#ifdef TSCH_CONF_ADDRESS_FILTER
#define TSCH_ADDRESS_FILTER TSCH_CONF_ADDRESS_FILTER
#else
#define TSCH_ADDRESS_FILTER 0
#endif /* TSCH_CONF_ADDRESS_FILTER */

#ifndef TSCH_802154_DUPLICATE_DETECTION
#ifdef TSCH_CONF_802154_DUPLICATE_DETECTION
#define TSCH_802154_DUPLICATE_DETECTION TSCH_CONF_802154_DUPLICATE_DETECTION
#else
#define TSCH_802154_DUPLICATE_DETECTION 1
#endif /* TSCH_CONF_802154_AUTOACK */
#endif /* TSCH_802154_AUTOACK */

#if TSCH_802154_DUPLICATE_DETECTION
struct seqno {
	rimeaddr_t sender;
	uint8_t seqno;
};

#ifdef NETSTACK_CONF_MAC_SEQNO_HISTORY
#define MAX_SEQNOS NETSTACK_CONF_MAC_SEQNO_HISTORY
#else /* NETSTACK_CONF_MAC_SEQNO_HISTORY */
#define MAX_SEQNOS 8
#endif /* NETSTACK_CONF_MAC_SEQNO_HISTORY */

static struct seqno received_seqnos[MAX_SEQNOS];
#endif /* TSCH_802154_DUPLICATE_DETECTION */

// variable to protect queue structure
volatile uint8_t working_on_queue;

#if ( QUEUEBUF_CONF_NUM && !(QUEUEBUF_CONF_NUM & (QUEUEBUF_CONF_NUM-1)) ) /* is it a power of two? */
#define NBR_BUFFER_SIZE QUEUEBUF_CONF_NUM // POWER OF 2 -- queue size
#else
#define NBR_BUFFER_SIZE 8
#endif /* !(QUEUEBUF_CONF_NUM & (QUEUEBUF_CONF_NUM-1)) */
#define macMinBE 1
#define macMaxFrameRetries 4
#define macMaxBE 4

// TSCH PACKET STRUCTURE
struct TSCH_packet
{
	struct queuebuf * pkt; // pointer to the packet to be sent
	uint8_t transmissions; // #transmissions performed for this packet
	mac_callback_t sent; // callback for this packet
	void *ptr; // parameters for MAC callback ... (usually NULL)
	uint8_t ret; //status -- MAC return code
};

struct neighbor_queue
{
	uint8_t time_source;
	uint8_t BE_value; // current value of backoff exponent
	uint8_t BW_value; // current value of backoff counter
	struct TSCH_packet buffer[NBR_BUFFER_SIZE]; // circular buffer of packets. Its size should be a power of two
	uint8_t put_ptr, get_ptr; // pointers for circular buffer implementation
};

/* NBR_TABLE_CONF_MAX_NEIGHBORS specifies the size of the table */
#include "net/nbr-table.h"
NBR_TABLE(struct neighbor_queue, neighbor_list);

static struct TSCH_packet *
get_next_packet_for_shared_tx(void);
struct neighbor_queue *
neighbor_queue_from_addr(const rimeaddr_t *addr);
struct neighbor_queue *
add_queue(const rimeaddr_t *addr);
int
remove_queue(const rimeaddr_t *addr);
int
add_packet_to_queue(mac_callback_t sent, void* ptr, const rimeaddr_t *addr);
int
remove_packet_from_queue(const rimeaddr_t *addr);
struct TSCH_packet*
read_packet_from_queue(const rimeaddr_t *addr);
static void
tsch_timer(void *ptr);

/** This function takes the MSB of gcc generated random number
 * because the LSB alone has very bad random characteristics,
 * while the MSB appears more random.
 * window is the upper limit of the number. It should be a power of two - 1
 **/
static uint8_t generate_random_byte(uint8_t window) {
	return (random_rand() >> 8) & window;
}

// This function returns a pointer to the queue of neighbor whose address is equal to addr
inline struct neighbor_queue *
neighbor_queue_from_addr(const rimeaddr_t *addr)
{
	struct neighbor_queue *n = nbr_table_get_from_lladdr(neighbor_list, addr);
	return n;
}

// This function adds one queue for neighbor whose address is equal to addr
// uses working_on_queue to protect data-structures from race conditions
// return 1 ok, 0 failed to allocate

struct neighbor_queue *
add_queue(const rimeaddr_t *addr)
{
	working_on_queue = 1;
	struct neighbor_queue *n;
	/* If we have an entry for this neighbor already, we renew it. */
	n = neighbor_queue_from_addr(addr);
	if (n == NULL) {
		n = nbr_table_add_lladdr(neighbor_list, addr);
	}
	//if n was actually allocated
	if (n) {
		/* Init neighbor entry */
		n->BE_value = macMinBE;
		n->BW_value = 0;
		n->put_ptr = 0;
		n->get_ptr = 0;
		n->time_source = 0;
		uint8_t i;
		for (i = 0; i < NBR_BUFFER_SIZE; i++) {
			n->buffer[i].pkt = 0;
			n->buffer[i].transmissions = 0;
		}
		working_on_queue = 0;
		return n;
	}
	working_on_queue = 0;
	return n;
}

// This function remove the queue of neighbor whose address is equal to addr
// uses working_on_queue to protect data-structures from race conditions
// return 1 ok, 0 failed to find the queue

int
remove_queue(const rimeaddr_t *addr)
{
	working_on_queue = 1;
	int i;
	struct neighbor_queue *n = neighbor_queue_from_addr(addr); // retrieve the queue from address
	if (n != NULL) {
		for (i = 0; i < NBR_BUFFER_SIZE; i++) {      // free packets of neighbor
			queuebuf_free(n->buffer[i].pkt);
		}
		nbr_table_remove(neighbor_list, n);
		working_on_queue = 0;
		return 1;
	}
	working_on_queue = 0;
	return 0;
}

// This function adds one packet to the queue of neighbor whose address is addr
// return 1 ok, 0 failed to allocate
// the packet to be inserted is in packetbuf
int
add_packet_to_queue(mac_callback_t sent, void* ptr, const rimeaddr_t *addr)
{
	struct neighbor_queue *n = neighbor_queue_from_addr(addr); // retrieve the queue from address
	if (n != NULL) {
		//is queue full?
		if (((n->put_ptr - n->get_ptr) & (NBR_BUFFER_SIZE - 1)) == (NBR_BUFFER_SIZE - 1)) {
			return 0;
		}
		n->buffer[n->put_ptr].pkt = queuebuf_new_from_packetbuf(); // create new packet from packetbuf
		n->buffer[n->put_ptr].sent = sent;
		n->buffer[n->put_ptr].ptr = ptr;
		n->buffer[n->put_ptr].ret = MAC_TX_DEFERRED;
		n->buffer[n->put_ptr].transmissions = 0;
		n->put_ptr = (n->put_ptr + 1) & (NBR_BUFFER_SIZE - 1);
		return 1;
	}
	return 0;
}

// This function removes the head-packet of the queue of neighbor whose address is addr
// return 1 ok, 0 failed
// remove one packet from the queue
int
remove_packet_from_queue(const rimeaddr_t *addr)
{
	struct neighbor_queue *n = neighbor_queue_from_addr(addr); // retrieve the queue from address
	if (n != NULL) {
		if (((n->put_ptr - n->get_ptr) & (NBR_BUFFER_SIZE - 1)) > 0) {
			queuebuf_free(n->buffer[n->get_ptr].pkt);
			n->get_ptr = (n->get_ptr + 1) & (NBR_BUFFER_SIZE - 1);
			return 1;
		} else {
			return 0;
		}
	}
	return 0;
}

// This function returns the first packet in the queue of neighbor whose address is addr
struct TSCH_packet*
read_packet_from_queue(const rimeaddr_t *addr)
{
	struct neighbor_queue *n = neighbor_queue_from_addr(addr); // retrieve the queue from address
	if (n != NULL) {
		if (((n->put_ptr - n->get_ptr) & (NBR_BUFFER_SIZE - 1)) > 0) {
			return &(n->buffer[n->get_ptr]);
		} else {
			return 0;
		}
	}
	return 0;
}

// This function returns the first packet in the queue of neighbor whose address is addr
struct TSCH_packet*
read_packet_from_neighbor_queue(const struct neighbor_queue *n)
{
	if (n != NULL) {
		if (((n->put_ptr - n->get_ptr) & (NBR_BUFFER_SIZE - 1)) > 0) {
			return &(n->buffer[n->get_ptr]);
		} else {
			return 0;
		}
	}
	return 0;
}

//this function is used to get a packet to send in a shared slot
static struct TSCH_packet *
get_next_packet_for_shared_slot_tx(void) {
	static struct neighbor_queue* last_neighbor_tx = NULL;
	if(last_neighbor_tx == NULL) {
		last_neighbor_tx = nbr_table_head(neighbor_list);
	}
	struct TSCH_packet * p = NULL;
	while(p==NULL && last_neighbor_tx != NULL) {
		p = read_packet_from_neighbor_queue( last_neighbor_tx );
		last_neighbor_tx = nbr_table_next(neighbor_list, last_neighbor_tx);
	}
	return p;
}
/*---------------------------------------------------------------------------*/
// Function send for TSCH-MAC, puts the packet in packetbuf in the MAC queue
static int
send_one_packet(mac_callback_t sent, void *ptr)
{
	//send_one_packet(sent, ptr);
	COOJA_DEBUG_STR("TSCH send_one_packet\n");

	uint16_t seqno;
	const rimeaddr_t *addr = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
	//Ask for ACK if we are sending anything other than broadcast
	if (!rimeaddr_cmp(addr, &rimeaddr_null)) {
		packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, 1);
	}
	/* PACKETBUF_ATTR_MAC_SEQNO cannot be zero, due to a peculiarity
	 in framer-802154.c. */
	seqno = (++ieee154e_vars.dsn) ? ieee154e_vars.dsn : ++ieee154e_vars.dsn;

	packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, seqno);
	if (NETSTACK_FRAMER.create() < 0) {
		return 0;
	}
	struct neighbor_queue *n;
	/* Look for the neighbor entry */
	n = neighbor_queue_from_addr(addr);
	if (n == NULL) {
		//add new neighbor to list of neighbors
		if (!add_queue(addr))
			return 0;
		//add new packet to neighbor list
		if (!add_packet_to_queue(sent, ptr, addr))
			return 0;
	} else {
		//add new packet to neighbor list
		if (!add_packet_to_queue(sent, ptr, addr))
			return 0;
	}
	return 1;
}
/*---------------------------------------------------------------------------*/
static void
send_packet(mac_callback_t sent, void *ptr)
{
	send_one_packet(sent, ptr);
}
/*---------------------------------------------------------------------------*/
static void
send_list(mac_callback_t sent, void *ptr, struct rdc_buf_list *buf_list)
{
	while (buf_list != NULL) {
		/* We backup the next pointer, as it may be nullified by
		 * mac_call_sent_callback() */
		struct rdc_buf_list *next = buf_list->next;
		int last_sent_ok;

		queuebuf_to_packetbuf(buf_list->buf);
		last_sent_ok = send_one_packet(sent, ptr);

		/* If packet transmission was not successful, we should back off and let
		 * upper layers retransmit, rather than potentially sending out-of-order
		 * packet fragments. */
		if (!last_sent_ok) {
			return;
		}
		buf_list = next;
	}
}
/*---------------------------------------------------------------------------*/
static void
packet_input(void)
{
	COOJA_DEBUG_STR("tsch packet_input begin\n");

	int original_datalen;
	uint8_t *original_dataptr;

	original_datalen = packetbuf_datalen();
	original_dataptr = packetbuf_dataptr();
#ifdef NETSTACK_DECRYPT
	NETSTACK_DECRYPT();
#endif /* NETSTACK_DECRYPT */

	if (NETSTACK_FRAMER.parse() < 0) {
		PRINTF("tsch: failed to parse %u\n", packetbuf_datalen());
#if TSCH_ADDRESS_FILTER
	} else if (!rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER),
					&rimeaddr_node_addr)
			&& !rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER),
					&rimeaddr_null)) {
		PRINTF("tsch: not for us\n");
#endif /* TSCH_ADDRESS_FILTER */
	} else {
		int duplicate = 0;

#if TSCH_802154_DUPLICATE_DETECTION
		/* Check for duplicate packet by comparing the sequence number
		 of the incoming packet with the last few ones we saw. */
		int i;
		for(i = 0; i < MAX_SEQNOS; ++i) {
			if(packetbuf_attr(PACKETBUF_ATTR_PACKET_ID) == received_seqnos[i].seqno &&
					rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_SENDER),
							&received_seqnos[i].sender)) {
				/* Drop the packet. */
				COOJA_DEBUG_STR("tsch: drop duplicate link layer packet");
				PRINTF("tsch: drop duplicate link layer packet %u\n",
						packetbuf_attr(PACKETBUF_ATTR_PACKET_ID));
				duplicate = 1;
			}
		}
		if(!duplicate) {
			for(i = MAX_SEQNOS - 1; i > 0; --i) {
				memcpy(&received_seqnos[i], &received_seqnos[i - 1],
						sizeof(struct seqno));
			}
			received_seqnos[0].seqno = packetbuf_attr(PACKETBUF_ATTR_PACKET_ID);
			rimeaddr_copy(&received_seqnos[0].sender,
					packetbuf_addr(PACKETBUF_ADDR_SENDER));
		}
#endif /* TSCH_802154_DUPLICATE_DETECTION */

		if (!duplicate) {
			NETSTACK_MAC.input();
			COOJA_DEBUG_STR("tsch packet_input, Not duplicate\n");
		}
	}
	COOJA_DEBUG_STR("tsch packet_input end\n");
}
/*---------------------------------------------------------------------------*/
static int
on(void)
{
	return NETSTACK_RADIO.on();
}
/*---------------------------------------------------------------------------*/
volatile static int keep_radio_on = 0;
static int
off(int set_keep_radio_on)
{
	keep_radio_on = set_keep_radio_on;
	if (keep_radio_on) {
		return NETSTACK_RADIO.on();
	} else {
		return NETSTACK_RADIO.off();
	}
}
/*---------------------------------------------------------------------------*/
static unsigned short
channel_check_interval(void)
{
	return 0;
}
/*---------------------------------------------------------------------------*/
#define BUSYWAIT_UNTIL_ABS(cond, t0, duration)                     	    \
  do { rtimer_clock_t now = RTIMER_NOW(), t1=t0+duration;               \
  	if((rtimer_clock_t)(t1-now)>duration) break;												\
    while(!(cond) && RTIMER_CLOCK_LT(now, t0));  												\
  } while(0)
/*---------------------------------------------------------------------------*/
#ifndef MIN
#define MIN(a, b) ((a) < (b)? (a) : (b))
#endif /* MIN */

/*---------------------------------------------------------------------------*/
static uint8_t
hop_channel(uint8_t offset)
{
	uint8_t channel = 11 + (offset + ieee154e_vars.asn) % 16;
	if ( NETSTACK_RADIO_set_channel(channel)) {
		return channel;
	}
	return 0;
}
/*---------------------------------------------------------------------------*/
PROCESS(tsch_tx_callback_process, "tsch_tx_callback_process");
/*---------------------------------------------------------------------------*/
static const rimeaddr_t BROADCAST_CELL_ADDRESS = { { 0, 0, 0, 0, 0, 0, 0, 0 } };
static const rimeaddr_t CELL_ADDRESS1 = { { 0x00, 0x12, 0x74, 01, 00, 01, 01, 01 } };
static const rimeaddr_t CELL_ADDRESS2 = { { 0x00, 0x12, 0x74, 02, 00, 02, 02, 02 } };
static const rimeaddr_t CELL_ADDRESS3 = { { 0x00, 0x12, 0x74, 03, 00, 03, 03, 03 } };


static const cell_t generic_shared_cell = { 0xffff, 0, LINK_OPTION_TX | LINK_OPTION_RX
		| LINK_OPTION_SHARED, LINK_TYPE_NORMAL, &BROADCAST_CELL_ADDRESS };

static const cell_t generic_eb_cell = { 0, 0, LINK_OPTION_TX, LINK_TYPE_ADVERTISING,
		&BROADCAST_CELL_ADDRESS };

static const cell_t cell_to_1 = { 1, 0, LINK_OPTION_TX | LINK_OPTION_RX
		| LINK_OPTION_SHARED | LINK_OPTION_TIME_KEEPING, LINK_TYPE_NORMAL,
		&CELL_ADDRESS1 };
static const cell_t cell_to_2 = { 2, 0, LINK_OPTION_TX | LINK_OPTION_RX
		| LINK_OPTION_SHARED, LINK_TYPE_NORMAL, &CELL_ADDRESS2 };
static const cell_t cell_to_3 = { 3, 0, LINK_OPTION_TX | LINK_OPTION_RX
		| LINK_OPTION_SHARED, LINK_TYPE_NORMAL, &CELL_ADDRESS3 };
static const cell_t cell_3_to_2 = { 4, 0, LINK_OPTION_TX | LINK_OPTION_RX
		| LINK_OPTION_SHARED, LINK_TYPE_NORMAL, &CELL_ADDRESS2 };

#define TOTAL_LINKS 6
#define TSCH_MIN_SIZE 6

static const cell_t * minimum_cells[6] = {
		&generic_eb_cell, &generic_shared_cell,	&generic_shared_cell,
		&generic_shared_cell, &generic_shared_cell,	&generic_shared_cell,
//		&cell_to_1, &cell_to_2, &cell_to_3, &cell_3_to_2
		};
static const cell_t * links_list[TOTAL_LINKS] = { &generic_eb_cell, &generic_shared_cell,
		&cell_to_1, &cell_to_2, &cell_to_3, &cell_3_to_2 };

static const slotframe_t minimum_slotframe = { 0, 101, 6, minimum_cells };

#include "dev/leds.h"
static slotframe_t const * current_slotframe;
static volatile struct pt mpt;
static volatile struct rtimer t;
static volatile rtimer_clock_t start;
#include "net/netstack.h"
volatile unsigned char we_are_sending = 0;
/*---------------------------------------------------------------------------*/
static cell_t *
get_cell(uint16_t timeslot)
{
	return (timeslot >= current_slotframe->on_size) ?
	NULL : current_slotframe->cells[timeslot];
}
/*---------------------------------------------------------------------------*/
static uint16_t
get_next_on_timeslot(uint16_t timeslot)
{
	return (timeslot >= current_slotframe->on_size - 1) ? 0 : timeslot + 1;
}
/*---------------------------------------------------------------------------*/
static int
powercycle(struct rtimer *t, void *ptr);
/* Schedule a wakeup from a reference time for a specific duration.
 * Provides basic protection against missed deadlines and timer overflows */
static uint8_t
schedule_fixed(struct rtimer *t, rtimer_clock_t ref_time,
		rtimer_clock_t duration)
{
	int r, ret = 1;
	rtimer_clock_t now = RTIMER_NOW() + 1;
	ref_time += duration;
	if (ref_time - now > duration) {
		COOJA_DEBUG_STR("schedule_fixed: missed deadline!\n");
		ref_time = RTIMER_NOW() + 5;
		ret = 0;
	}

	r = rtimer_set(t, ref_time, 1, (void
	(*)(struct rtimer *, void *)) powercycle, NULL);
	if (r != RTIMER_OK) {
		COOJA_DEBUG_STR("schedule_fixed: could not set rtimer\n");
		ret *= 2;
	}
	return ret;
}
/*---------------------------------------------------------------------------*/
static volatile uint8_t waiting_for_radio_interrupt = 0;
static volatile uint8_t need_ack;
static volatile struct received_frame_s *last_rf;
static volatile int16_t last_drift;
/*---------------------------------------------------------------------------*/
void
tsch_resume_powercycle(uint8_t is_ack, uint8_t need_ack_irq, struct received_frame_s * last_rf_irq)
{
	need_ack = need_ack_irq;
	last_rf = last_rf_irq;
	if (waiting_for_radio_interrupt || NETSTACK_RADIO_get_rx_end_time() != 0) {
		waiting_for_radio_interrupt = 0;
		schedule_fixed(&t, RTIMER_NOW(), 5);
	}
	leds_off(LEDS_RED);
}
/*---------------------------------------------------------------------------*/
static int
powercycle(struct rtimer *t, void *ptr)
{
	/* if timeslot for tx, and we have a packet, call timeslot_tx
	 * else if timeslot for rx, call timeslot_rx
	 * otherwise, schedule next wakeup
	 */
	PT_BEGIN(&mpt);
	static volatile uint16_t timeslot = 0;
	static volatile int32_t drift_correction = 0;
	static volatile int32_t drift = 0; //estimated drift to all time source neighbors
	static volatile uint16_t drift_counter = 0; //number of received drift corrections source neighbors
	static uint8_t cell_decison = 0;
	static cell_t * cell = NULL;
	static struct TSCH_packet* p = NULL;
	static struct neighbor_queue *n = NULL;
	start = RTIMER_NOW();
	//while MAC-RDC is not disabled, and while its synchronized
	while (ieee154e_vars.is_sync && ieee154e_vars.state != TSCH_OFF) {
		COOJA_DEBUG_STR("Cell start\n");
		/* sync with cycle start and enable capturing start & end sfd*/
		NETSTACK_RADIO_sfd_sync(1, 1);
		leds_on(LEDS_GREEN);
		cell = get_cell(timeslot);
		if (cell == NULL || working_on_queue) {
			COOJA_DEBUG_STR("Off CELL\n");
			//off cell
			off(keep_radio_on);
			cell_decison = CELL_OFF;
		} else {
			hop_channel(cell->channel_offset);
			p = NULL;
			n = NULL;
			last_drift=0;
			last_rf = NULL;
			need_ack = 0;
			waiting_for_radio_interrupt = 0;
			//is there a packet to send? if not check if this slot is RX too
			if (cell->link_options & LINK_OPTION_TX) {
				//is it for ADV/EB?
				if (cell->link_type == LINK_TYPE_ADVERTISING) {
					//TODO fetch adv/EB packets
				} else { //NORMAL link
					//pick a packet from the neighbors queue who is associated with this cell
					n = neighbor_queue_from_addr(cell->node_address);
					if (n != NULL) {
						p = read_packet_from_neighbor_queue(n);
						//if there it is a shared broadcast slot and there were no broadcast packets, pick any unicast packet
						if(p==NULL && rimeaddr_cmp(cell->node_address, &BROADCAST_CELL_ADDRESS) && (cell->link_options & LINK_OPTION_SHARED)) {
							p = get_next_packet_for_shared_slot_tx();
						}
					}
				}
			}

			if(cell->link_options & LINK_OPTION_TX) {
				if(p != NULL) {
					// if dedicated slot or shared slot and BW_value=0, we transmit the packet
					if(!(cell->link_options & LINK_OPTION_SHARED)
						|| n->BW_value == 0) {
						cell_decison = CELL_TX;
					} else if(n->BW_value != 0) {
						// packet to transmit but we cannot use shared slot due to backoff counter
						n->BW_value--;
						cell_decison = CELL_TX_BACKOFF;
					}
				} else {
					cell_decison = CELL_TX_IDLE;
				}
			}

			if( (cell->link_options & LINK_OPTION_RX) && cell_decison != CELL_TX) {
				cell_decison = CELL_RX;
			}

			if(cell_decison != CELL_TX && cell_decison != CELL_RX) {
				COOJA_DEBUG_STR("Nothing to TX or RX --> off CELL\n");
				off(keep_radio_on);
			} else if (cell_decison == CELL_TX) {
				COOJA_DEBUG_STR("CELL_TX");
				//timeslot_tx(t, start, packet, packet_len);
				static void * payload = NULL;
				static unsigned short payload_len = 0;
				payload = queuebuf_dataptr(p->pkt);
				payload_len = queuebuf_datalen(p->pkt);
				//TODO There are small timing variations visible in cooja, which needs tuning
				static uint8_t is_broadcast = 0, len, seqno, ret;
				uint16_t ack_sfd_time = 0;
				rtimer_clock_t ack_sfd_rtime = 0;
				is_broadcast = rimeaddr_cmp(queuebuf_addr(p->pkt, PACKETBUF_ADDR_RECEIVER), &rimeaddr_null);
				we_are_sending = 1;
				char* payload_ptr = payload;
				//read seqno from payload!
				seqno = payload_ptr[2];
				//prepare packet to send
				uint8_t success = !NETSTACK_RADIO.prepare(payload, payload_len);
				uint8_t cca_status = 1;
#if CCA_ENABLED
				//delay before CCA
				schedule_fixed(t, start, TsCCAOffset);
				PT_YIELD(&mpt);
				on();
				//CCA
				BUSYWAIT_UNTIL_ABS(!(cca_status |= NETSTACK_RADIO.channel_clear()),
						start, TsCCAOffset + TsCCA);
				//there is not enough time to turn radio off
				off(keep_radio_on);
#endif /* CCA_ENABLED */
				if (cca_status == 0) {
					success = RADIO_TX_COLLISION;
				} else {
					//delay before TX
					NETSTACK_RADIO_sfd_sync(0, 1);
					schedule_fixed(t, start, TsTxOffset - delayTx);
					PT_YIELD(&mpt);
					//to record the duration of packet tx
					static rtimer_clock_t tx_time;
					tx_time = RTIMER_NOW();
					//send packet already in radio tx buffer
					success = NETSTACK_RADIO.transmit(payload_len);
					tx_time = NETSTACK_RADIO_read_sfd_timer() - tx_time;
					//limit tx_time in case of something wrong
					tx_time = MIN(tx_time, wdDataDuration);
					off(keep_radio_on);

					if (success == RADIO_TX_OK) {
						if (!is_broadcast) {
							//wait for ack: after tx
							COOJA_DEBUG_STR("wait for ACK\n");
							schedule_fixed(t, start,
									TsTxOffset + tx_time + TsTxAckDelay - TsShortGT - delayRx);
							/* disable capturing sfd */
							NETSTACK_RADIO_sfd_sync(0, 0);
							PT_YIELD(&mpt);
							COOJA_DEBUG_STR("wait for detecting ACK\n");
							waiting_for_radio_interrupt = 1;
							on();
							cca_status = NETSTACK_RADIO.receiving_packet()
									|| NETSTACK_RADIO.pending_packet()
									|| !NETSTACK_RADIO.channel_clear();
							if (!cca_status) {
								schedule_fixed(t, start,
										TsTxOffset + tx_time + TsTxAckDelay + TsShortGT);
								PT_YIELD(&mpt);
								cca_status |= NETSTACK_RADIO.receiving_packet()
										|| NETSTACK_RADIO.pending_packet()
										|| !NETSTACK_RADIO.channel_clear();
							}
							if (cca_status) {
								COOJA_DEBUG_STR("ACK detected\n");
								uint8_t ackbuf[ACK_LEN + EXTRA_ACK_LEN];
								if (!NETSTACK_RADIO.pending_packet()) {
									COOJA_DEBUG_STR("not pending_packet\n");
									schedule_fixed(t, start,
											TsTxOffset + tx_time + TsTxAckDelay + TsShortGT
													+ wdAckDuration);
									PT_YIELD(&mpt);
								}
								//is there an ACK pending?
								if (NETSTACK_RADIO.pending_packet()) {
									COOJA_DEBUG_STR("ACK Read:\n");
									len = NETSTACK_RADIO.read(ackbuf, ACK_LEN + EXTRA_ACK_LEN);
								} else if (NETSTACK_RADIO_pending_irq()) {
									//we have received something in radio FIFO but radio interrupt has not fired because we are inside rtimer code
									len = NETSTACK_RADIO_read_ack(ackbuf, ACK_LEN + EXTRA_ACK_LEN);
								}
								if (2 == ackbuf[0] && len >= ACK_LEN && seqno == ackbuf[2]) {
									success = RADIO_TX_OK;
									uint16_t ack_status = 0;
									if (ackbuf[1] & 2) { //IE-list present?
										COOJA_DEBUG_STR("ACK IE-list present");

										if (len == ACK_LEN + EXTRA_ACK_LEN) {
											COOJA_DEBUG_STR("ACK_LEN + EXTRA_ACK_LEN");

											if (ackbuf[3] == 0x02 && ackbuf[4] == 0x1e) {
												COOJA_DEBUG_STR("ACK sync header");

												ack_status = ackbuf[5];
												ack_status |= ackbuf[6] << 8;
												/* If the originator was a time source neighbor, the receiver adjusts its own clock by incorporating the
												 * 	difference into an average of the drift to all its time source neighbors. The averaging method is
												 * 	implementation dependent. If the receiver is not a clock source, the time correction is ignored.
												 */
												if (n->time_source) {
													COOJA_DEBUG_STR("ACK from time_source");

													/* extract time correction */
													int16_t d=0;
													//is it a negative correction?
													if(ack_status & 0x0800) {
														d = -(ack_status & 0x0fff & ~0x0800);
													} else {
														d = ack_status & 0x0fff;
													}
													drift += d;
													drift_counter++;
												}
												if (ack_status & NACK_FLAG) {
													//TODO return NACK status to upper layer
													COOJA_DEBUG_STR("ACK NACK_FLAG\n");
												}
											}

										}
									}
									COOJA_DEBUG_STR("ACK ok\n");
								} else {
									success = RADIO_TX_NOACK;
									COOJA_DEBUG_STR("ACK not ok!\n");
								}
							} else {
								COOJA_DEBUG_STR("No ack!\n");
								success = RADIO_TX_NOACK;

							}
							waiting_for_radio_interrupt = 0;
						}
						we_are_sending = 0;
						off(keep_radio_on);
						COOJA_DEBUG_STR("end tx slot\n");
					}
				}

				if (success == RADIO_TX_NOACK) {
					p->transmissions++;
					if (p->transmissions == macMaxFrameRetries) {
						remove_packet_from_queue(queuebuf_addr(p->pkt, PACKETBUF_ADDR_RECEIVER));
						n->BE_value = macMinBE;
						n->BW_value = 0;
					}
					if ((cell->link_options & LINK_OPTION_SHARED) && !is_broadcast) {
						uint8_t window = 1 << n->BE_value;
						n->BW_value = generate_random_byte(window - 1);
						n->BE_value++;
						if (n->BE_value > macMaxBE) {
							n->BE_value = macMaxBE;
						}
					}
					ret = MAC_TX_NOACK;
				} else if (success == RADIO_TX_OK) {
					remove_packet_from_queue(queuebuf_addr(p->pkt, PACKETBUF_ADDR_RECEIVER));
					if (!read_packet_from_queue(cell->node_address)) {
						// if no more packets in the queue
						n->BW_value = 0;
						n->BE_value = macMinBE;
					} else {
						// if queue is not empty
						n->BW_value = 0;
					}
					ret = MAC_TX_OK;
				} else if (success == RADIO_TX_COLLISION) {
					p->transmissions++;
					if (p->transmissions == macMaxFrameRetries) {
						remove_packet_from_queue(queuebuf_addr(p->pkt, PACKETBUF_ADDR_RECEIVER));
						n->BE_value = macMinBE;
						n->BW_value = 0;
					}
					if ((cell->link_options & LINK_OPTION_SHARED) && !is_broadcast) {
						uint8_t window = 1 << n->BE_value;
						n->BW_value = generate_random_byte(window - 1);
						n->BE_value++;
						if (n->BE_value > macMaxBE) {
							n->BE_value = macMaxBE;
						}
					}
					ret = MAC_TX_COLLISION;
				} else if (success == RADIO_TX_ERR) {
					p->transmissions++;
					if (p->transmissions == macMaxFrameRetries) {
						remove_packet_from_queue(queuebuf_addr(p->pkt, PACKETBUF_ADDR_RECEIVER));
						n->BE_value = macMinBE;
						n->BW_value = 0;
					}
					if ((cell->link_options & LINK_OPTION_SHARED) && !is_broadcast) {
						uint8_t window = 1 << n->BE_value;
						n->BW_value = generate_random_byte(window - 1);
						n->BE_value++;
						if (n->BE_value > macMaxBE) {
							n->BE_value = macMaxBE;
						}
					}
					ret = MAC_TX_ERR;
				} else {
					// successful transmission
					remove_packet_from_queue(queuebuf_addr(p->pkt, PACKETBUF_ADDR_RECEIVER));
					if (!read_packet_from_queue(cell->node_address)) {
						// if no more packets in the queue
						n->BW_value = 0;
						n->BE_value = macMinBE;
					} else {
						// if queue is not empty
						n->BW_value = 0;
					}
					ret = MAC_TX_OK;
				}
				/* poll MAC TX callback */
				p->ret=ret;
				process_post(&tsch_tx_callback_process, PROCESS_EVENT_POLL, p);
			} else if (cell_decison == CELL_RX) {
//				timeslot_rx(t, start, msg, MSG_LEN);
				if (cell->link_options & LINK_OPTION_TIME_KEEPING) {
					// TODO
				}
				{
					//TODO There are small timing variations visible in cooja, which needs tuning
					static uint8_t is_broadcast = 0, len, seqno, ret;
					uint16_t ack_sfd_time = 0;
					rtimer_clock_t ack_sfd_rtime = 0;

					is_broadcast = rimeaddr_cmp(cell->node_address, &rimeaddr_null);

					//wait before RX
					schedule_fixed(t, start, TsTxOffset - TsLongGT);
					COOJA_DEBUG_STR("schedule RX on guard time - TsLongGT");
					PT_YIELD(&mpt);
					//Start radio for at least guard time
					on();
					COOJA_DEBUG_STR("RX on -TsLongGT");
					uint8_t cca_status = 0;
					cca_status = (!NETSTACK_RADIO.channel_clear()
							|| NETSTACK_RADIO.pending_packet()
							|| NETSTACK_RADIO.receiving_packet());
					//Check if receiving within guard time
					schedule_fixed(t, start, TsTxOffset + TsLongGT);
					PT_YIELD(&mpt);
					COOJA_DEBUG_STR("RX on +TsLongGT");

					if (!(NETSTACK_RADIO_get_rx_end_time() || cca_status || NETSTACK_RADIO.pending_packet()
							|| !NETSTACK_RADIO.channel_clear()
							|| NETSTACK_RADIO.receiving_packet())) {
						COOJA_DEBUG_STR("RX no packet in air\n");
						off(keep_radio_on);
						//no packets on air
						ret = 0;
					} else {
//						if (NETSTACK_RADIO_get_rx_end_time() == 0 && (!NETSTACK_RADIO.pending_packet())) {
//							//wait until rx finishes
//							schedule_fixed(t, start, TsTxOffset + wdDataDuration);
//							waiting_for_radio_interrupt = 1;
//							COOJA_DEBUG_STR("Wait until RX is done");
//							PT_YIELD(&mpt);
//						}

						uint16_t expected_rx = start + TsTxOffset;
						uint16_t rx_duration = NETSTACK_RADIO_get_rx_end_time() - (start + TsTxOffset);
						off(keep_radio_on);

						/* wait until ack time */
						if (need_ack) {
							schedule_fixed(t, NETSTACK_RADIO_get_rx_end_time(), TsTxAckDelay - delayTx);
							PT_YIELD(&mpt);
							COOJA_DEBUG_STR("send_ack()");
							NETSTACK_RADIO_send_ack();
						}
						/* If the originator was a time source neighbor, the receiver adjusts its own clock by incorporating the
						 * 	difference into an average of the drift to all its time source neighbors. The averaging method is
						 * 	implementation dependent. If the receiver is not a clock source, the time correction is ignored.
						 */
						//drift calculated in radio_interrupt
						if (last_drift) {
							COOJA_DEBUG_PRINTF("drift seen %d\n", last_drift);
							// check the source address for potential time-source match
							n = neighbor_queue_from_addr(&last_rf->source_address);
							if(n != NULL && n->time_source) {
								// should be the average of drifts to all time sources
								drift_correction -= last_drift;
								++drift_counter;
								COOJA_DEBUG_STR("drift recorded");
							}
						}
						//XXX return length instead? or status? or something?
						ret = 1;
					}
				}
			}
		}
		uint16_t dt, duration, next_timeslot;
		next_timeslot = get_next_on_timeslot(timeslot);
		dt =
				next_timeslot ? next_timeslot - timeslot :
						current_slotframe->length - timeslot;
		duration = dt * TsSlotDuration;

		/* apply sync correction on the start of the new slotframe */
		if (!next_timeslot) {
			if(drift_counter) {
				/* convert from microseconds to rtimer ticks and take average */
				drift_correction += (drift*100)/(3051*drift_counter);
			}
			if(drift_correction) {
				COOJA_DEBUG_PRINTF("New slot frame: drift_correction %d", drift_correction);
			}	else {
				COOJA_DEBUG_STR("New slot frame");
			}
			duration += (int16_t)drift_correction;
			drift_correction = 0;
			drift=0;
			drift_counter=0;
		}
		timeslot = next_timeslot;
		ieee154e_vars.asn += dt;
		start += duration;

		/* check for missed deadline and skip slot accordingly in order not to corrupt the whole schedule */
		if (start - RTIMER_NOW() > duration) {
			COOJA_DEBUG_STR("skipping slot because of missed deadline!\n");
			//go for next slot then
			next_timeslot = get_next_on_timeslot(timeslot);
			dt =
					next_timeslot ? next_timeslot - timeslot :
							current_slotframe->length - timeslot;
			uint16_t duration2 = dt * TsSlotDuration;
			timeslot = next_timeslot;
			ieee154e_vars.asn += dt;
			schedule_fixed(t, start-duration, duration + duration2);
			start += duration2;
		} else {
			schedule_fixed(t, start-duration, duration);
		}

		leds_off(LEDS_GREEN);
		PT_YIELD(&mpt);
}
COOJA_DEBUG_STR("TSCH is OFF!!");
PT_END(&mpt);
}
/*---------------------------------------------------------------------------*/
/* This function adds the Sync IE from the beginning of the buffer and returns the reported drift in microseconds */
static int16_t
add_sync_IE(uint8_t* buf, int32_t time_difference_32, uint8_t nack) {
	int16_t time_difference;
	uint16_t ack_status = 0;
	uint8_t len=4;
	//do the math in 32bits to save precision
	time_difference = time_difference_32 = (time_difference_32 * 3051)/100;
	if(time_difference >=0) {
		ack_status=time_difference & 0x07ff;
	} else {
		ack_status=((-time_difference) & 0x07ff) | 0x0800;
	}
	if(nack) {
		ack_status |= 0x8000;
	}
	buf[0] = 0x02;
	buf[1] = 0x1e;
	buf[2] = ack_status & 0xff;
	buf[3] = (ack_status >> 8) & 0xff;
	return time_difference;
}
/*---------------------------------------------------------------------------*/
// TODO Create an EB packet and puts it in the EB queue
static int
send_eb(rimeaddr_t *addr, int16_t reported_drift, slotframe_t* slotframe, cell_t * links_list, uint8_t links_list_size)
{
	uint8_t* buf;
	uint16_t seqno;
	uint8_t nack = 0;

	//send_one_packet(sent, ptr);
	COOJA_DEBUG_STR("TSCH send_one_packet\n");
	packetbuf_clear();
	buf = (uint8_t*)packetbuf_dataptr();
	//default is broadcast
	//could be unicast if sent as a reply to a specific EB request
	packetbuf_set_addr(PACKETBUF_ADDR_RECEIVER, &rimeaddr_null);
//	const rimeaddr_t *addr = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
	//Ask for ACK if we are sending anything otherthan broadcast
	if (!rimeaddr_cmp(addr, &rimeaddr_null)) {
		packetbuf_set_attr(PACKETBUF_ATTR_MAC_ACK, 1);
	}
	/* PACKETBUF_ATTR_MAC_SEQNO cannot be zero, due to a pecuilarity
	 in framer-802154.c. */
	seqno = (++ieee154e_vars.mac_ebsn) ? ieee154e_vars.mac_ebsn : ++ieee154e_vars.mac_ebsn;
	packetbuf_set_attr(PACKETBUF_ATTR_MAC_SEQNO, seqno);
	if (NETSTACK_FRAMER.create() < 0) {
		return 0;
	}
	//fcf
	buf[0] = 0x02;
	buf[1] = 0x22; //b9:IE-list-present=1 - b12-b13:frame version=2
	buf[2] = ieee154e_vars.mac_ebsn;
	/* Append IE timesync */
	//Put IEs: sync, slotframe and link, timeslot and channel hopping sequence
	if (reported_drift != 0) {
		add_sync_IE(buf+3, reported_drift, nack);
	}

	if (slotframe != NULL) {
		if (links_list_size && links_list != NULL) {

		}
	}
	return 1;
}
/*---------------------------------------------------------------------------*/
void
tsch_associate(void)
{
	/* TODO Synchronize
	 * If we are a master, start right away
	 * otherwise, wait for EBs to associate with a master
	 */
	COOJA_DEBUG_STR("tsch_associate\n");
	waiting_for_radio_interrupt = 0;
	we_are_sending = 0;
	//for now assume we are in sync
	ieee154e_vars.is_sync = 1;
	//something other than 0 for now
	ieee154e_vars.state = TSCH_ASSOCIATED;
	//process the schedule, to create queues and find time-sources (time-keeping)
	if(!working_on_queue) {
		struct neighbor_queue *n;
		uint8_t i = 0;
		for(i=0; i<TOTAL_LINKS; i++) {
			//add queues for neighbors with tx links and for time-sources
			if( (links_list[i]->link_options & LINK_OPTION_TIME_KEEPING)
					|| (links_list[i]->link_options & LINK_OPTION_TX) ) {
				rimeaddr_t *addr = links_list[i]->node_address;
				/* Look for the neighbor entry */
				n = neighbor_queue_from_addr(addr);
				if (n == NULL) {
					//add new neighbor to list of neighbors
					n=add_queue(addr);
				}
				if( n!= NULL ) {
					n->time_source = (links_list[i]->link_options & LINK_OPTION_TIME_KEEPING) ? 1 : n->time_source;
				}
			}
		}
	}
	start = RTIMER_NOW();
	schedule_fixed(&t, start, TsSlotDuration);
}
/*---------------------------------------------------------------------------*/
volatile uint8_t ackbuf[1+ACK_LEN + EXTRA_ACK_LEN]={0};
void tsch_make_sync_ack(uint8_t **buf, uint8_t seqno, rtimer_clock_t last_packet_timestamp, uint8_t nack) {
	int32_t time_difference_32;
	COOJA_DEBUG_STR("tsch_make_sync_ack");
	*buf=ackbuf;
	/* calculating sync in rtimer ticks */
	time_difference_32 = (int32_t)start + TsTxOffset - last_packet_timestamp;
	last_drift = time_difference_32;
	/* ackbuf[1+ACK_LEN + EXTRA_ACK_LEN] = {ACK_LEN + EXTRA_ACK_LEN + AUX_LEN, 0x02, 0x00, seqno, 0x02, 0x1e, ack_status_LSB, ack_status_MSB}; */
	ackbuf[1] = 0x02; /* ACK frame */
	ackbuf[2] = 0x22; /* b9:IE-list-present=1 - b12-b13:frame version=2 */
	ackbuf[3] = seqno;
	/* Append IE timesync */
	add_sync_IE(&ackbuf[4], time_difference_32, nack);
	ackbuf[0] = 3 /*FCF 2B + SEQNO 1B*/ + 4 /* sync IE size */;
}
/*---------------------------------------------------------------------------*/
static void
init(void)
{
	current_slotframe = &minimum_slotframe;
	ieee154e_vars.asn = 0;
	ieee154e_vars.captured_time = 0;
	ieee154e_vars.dsn = 0;
	ieee154e_vars.is_sync = 0;
	ieee154e_vars.state = 0;
	ieee154e_vars.sync_timeout = 0; //30sec/slotDuration - (asn-asn0)*slotDuration
	ieee154e_vars.mac_ebsn = 0;
	ieee154e_vars.join_priority = 0xff; /* inherit from RPL - PAN coordinator: 0 -- lower is better */
	nbr_table_register(neighbor_list, NULL);
	working_on_queue = 0;
	softack_make_callback_f *softack_make = tsch_make_sync_ack;
	softack_interrupt_exit_callback_f *interrupt_exit = tsch_resume_powercycle;
	NETSTACK_RADIO_softack_subscribe(softack_make, interrupt_exit);

	//schedule next wakeup? or leave for higher layer to decide? i.e, scan, ...
	tsch_associate();
}
/*---------------------------------------------------------------------------*/
/* a polled-process to invoke the MAC tx callback asynchronously */
PROCESS_THREAD(tsch_tx_callback_process, ev, data)
{
	int len;
	PROCESS_BEGIN();
	PRINTF("tsch_tx_callback_process: started\n");
	while (1) {
		PROCESS_YIELD_UNTIL(ev == PROCESS_EVENT_POLL);

		PRINTF("tsch_tx_callback_process: calling mac tx callback\n");
		COOJA_DEBUG_STR("tsch_tx_callback_process: calling mac tx callback\n");
		if(data != NULL) {
			struct TSCH_packet* p = (struct TSCH_packet*) data;
			/* XXX callback -- do we need to restore the packet to packetbuf? */
			mac_call_sent_callback(p->sent, p->ptr, p->ret, p->transmissions);
		}
	}
	PROCESS_END();
}
/*---------------------------------------------------------------------------*/
const struct rdc_driver tschrdc_driver = {
	"tschrdc",
	init,
	send_packet,
	send_list,
	packet_input,
	on,
	off,
	channel_check_interval,
};
/*---------------------------------------------------------------------------*/
