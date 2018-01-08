/* Copyright (c) 2013, Linaro Limited
 * Copyright (c) 2013, Nokia Solutions and Networks
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include "config.h"

#include <odp_api.h>
#include <odp_packet_internal.h>
#include <odp_packet_io_internal.h>
#include <odp_classification_internal.h>
#include <odp_debug_internal.h>
#include <odp/api/hints.h>
#include <odp_queue_if.h>
#include <odp_pktio_ops_loopback.h>

#include <protocols/eth.h>
#include <protocols/ip.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/if_ether.h>

/* MAC address for the "loop" interface */
static const char pktio_loop_mac[] = {0x02, 0xe9, 0x34, 0x80, 0x73, 0x01};

static int loopback_stats_reset(pktio_entry_t *pktio_entry);

static int loopback_open(odp_pktio_t id, pktio_entry_t *pktio_entry,
			 const char *devname, odp_pool_t pool ODP_UNUSED)
{
	pktio_ops_loopback_data_t *pkt_lbk = NULL;

	if (strcmp(devname, "loop"))
		return -1;

	pktio_entry->s.ops_data = ODP_OPS_DATA_ALLOC(sizeof(*pkt_lbk));
	if (odp_unlikely(pktio_entry->s.ops_data == NULL)) {
		ODP_ERR("Failed to allocate pktio_ops_loopback_data_t struct");
		return -1;
	}

	pkt_lbk = pktio_entry->s.ops_data;
	memset(pkt_lbk, 0, sizeof(*pkt_lbk));

	char loopq_name[ODP_QUEUE_NAME_LEN];

	snprintf(loopq_name, sizeof(loopq_name), "%" PRIu64 "-pktio_loopq",
		 odp_pktio_to_u64(id));
	pkt_lbk->loopq = odp_queue_create(loopq_name, NULL);

	if (pkt_lbk->loopq == ODP_QUEUE_INVALID) {
		ODP_OPS_DATA_FREE(pktio_entry->s.ops_data);
		return -1;
	}

	odp_ticketlock_init(&pkt_lbk->rx_lock);
	odp_ticketlock_init(&pkt_lbk->tx_lock);

	loopback_stats_reset(pktio_entry);

	return 0;
}

static int loopback_close(pktio_entry_t *pktio_entry)
{
	int result = 0;
	pktio_ops_loopback_data_t *pkt_lbk = pktio_entry->s.ops_data;

	if (odp_queue_destroy(pkt_lbk->loopq))
		result = -1;

	if (ODP_OPS_DATA_FREE(pktio_entry->s.ops_data))
		result = -1;

	return result;
}

static int loopback_recv(pktio_entry_t *pktio_entry, int index ODP_UNUSED,
			 odp_packet_t pkts[], int len)
{
	int nbr, i;
	odp_buffer_hdr_t *hdr_tbl[QUEUE_MULTI_MAX];
	queue_t queue;
	odp_packet_hdr_t *pkt_hdr;
	odp_packet_t pkt;
	odp_time_t ts_val;
	odp_time_t *ts = NULL;
	int num_rx = 0;
	pktio_ops_loopback_data_t *pkt_lbk = pktio_entry->s.ops_data;

	if (odp_unlikely(len > QUEUE_MULTI_MAX))
		len = QUEUE_MULTI_MAX;

	odp_ticketlock_lock(&pkt_lbk->rx_lock);

	queue = queue_fn->from_ext(pkt_lbk->loopq);
	nbr = queue_fn->deq_multi(queue, hdr_tbl, len);

	if (pkt_lbk->pktin_cfg.bit.ts_all || pkt_lbk->pktin_cfg.bit.ts_ptp) {
		ts_val = odp_time_global();
		ts = &ts_val;
	}

	for (i = 0; i < nbr; i++) {
		pkt = packet_from_buf_hdr(hdr_tbl[i]);
		pkt_hdr = odp_packet_hdr(pkt);

		packet_set_ts(pkt_hdr, ts);

		pktio_entry->s.stats.in_octets += odp_packet_len(pkt);
		pkts[num_rx++] = pkt;
	}

	pktio_entry->s.stats.in_ucast_pkts += num_rx;

	odp_ticketlock_unlock(&pkt_lbk->rx_lock);

	return num_rx;
}

static int loopback_send(pktio_entry_t *pktio_entry, int index ODP_UNUSED,
			 const odp_packet_t pkt_tbl[], int len)
{
	odp_buffer_hdr_t *hdr_tbl[QUEUE_MULTI_MAX];
	queue_t queue;
	int i;
	int ret;
	uint32_t bytes = 0;
	pktio_ops_loopback_data_t *pkt_lbk = pktio_entry->s.ops_data;

	if (odp_unlikely(len > QUEUE_MULTI_MAX))
		len = QUEUE_MULTI_MAX;

	for (i = 0; i < len; ++i) {
		hdr_tbl[i] = packet_to_buf_hdr(pkt_tbl[i]);
		bytes += odp_packet_len(pkt_tbl[i]);
	}

	if (pktio_entry->s.config.outbound_ipsec)
		for (i = 0; i < len; ++i) {
			odp_buffer_t buf = buf_from_buf_hdr(hdr_tbl[i]);
			odp_ipsec_packet_result_t result;

			if (_odp_buffer_event_subtype(buf) !=
			    ODP_EVENT_PACKET_IPSEC)
				continue;

			/* Possibly postprocessing packet */
			odp_ipsec_result(&result, pkt_tbl[i]);

			_odp_buffer_event_subtype_set(buf,
						      ODP_EVENT_PACKET_BASIC);
		}

	odp_ticketlock_lock(&pkt_lbk->tx_lock);

	queue = queue_fn->from_ext(pkt_lbk->loopq);
	ret = queue_fn->enq_multi(queue, hdr_tbl, len);

	if (ret > 0) {
		pktio_entry->s.stats.out_ucast_pkts += ret;
		pktio_entry->s.stats.out_octets += bytes;
	} else {
		ODP_DBG("queue enqueue failed %i\n", ret);
		ret = -1;
	}

	odp_ticketlock_unlock(&pkt_lbk->tx_lock);

	return ret;
}

static uint32_t loopback_mtu_get(pktio_entry_t *pktio_entry ODP_UNUSED)
{
	/* the loopback interface imposes no maximum transmit size limit */
	return INT_MAX;
}

static int loopback_mac_addr_get(pktio_entry_t *pktio_entry ODP_UNUSED,
				 void *mac_addr)
{
	memcpy(mac_addr, pktio_loop_mac, ETH_ALEN);
	return ETH_ALEN;
}

static int loopback_link_status(pktio_entry_t *pktio_entry ODP_UNUSED)
{
	/* loopback interfaces are always up */
	return 1;
}

static int loopback_config(pktio_entry_t *pktio_entry,
			   const odp_pktio_config_t *p)
{
	pktio_ops_loopback_data_t *pkt_lbk = pktio_entry->s.ops_data;

	/* Copy the configuration into pkt I/O structure. */
	pkt_lbk->pktin_cfg = p->pktin;

	return 0;
}

static int loopback_capability(pktio_entry_t *pktio_entry ODP_UNUSED,
			       odp_pktio_capability_t *capa)
{
	memset(capa, 0, sizeof(odp_pktio_capability_t));

	capa->max_input_queues  = 1;
	capa->max_output_queues = 1;
	capa->set_op.op.promisc_mode = 1;

	odp_pktio_config_init(&capa->config);
	capa->config.pktin.bit.ts_all = 1;
	capa->config.pktin.bit.ts_ptp = 1;
	capa->config.inbound_ipsec = 1;
	capa->config.outbound_ipsec = 1;

	return 0;
}

static int loopback_promisc_mode_set(pktio_entry_t *pktio_entry,
				     odp_bool_t enable)
{
	pktio_ops_loopback_data_t *pkt_lbk = pktio_entry->s.ops_data;

	pkt_lbk->promisc = enable;
	return 0;
}

static int loopback_promisc_mode_get(pktio_entry_t *pktio_entry)
{
	pktio_ops_loopback_data_t *pkt_lbk = pktio_entry->s.ops_data;

	return pkt_lbk->promisc ? 1 : 0;
}

static int loopback_stats(pktio_entry_t *pktio_entry,
			  odp_pktio_stats_t *stats)
{
	memcpy(stats, &pktio_entry->s.stats, sizeof(odp_pktio_stats_t));
	return 0;
}

static int loopback_stats_reset(pktio_entry_t *pktio_entry ODP_UNUSED)
{
	memset(&pktio_entry->s.stats, 0, sizeof(odp_pktio_stats_t));
	return 0;
}

static int loop_init_global(void)
{
	ODP_PRINT("PKTIO: initialized loop interface.\n");
	return 0;
}

static pktio_ops_module_t loopback_pktio_ops = {
	.base = {
		.name = "loop",
		.init_local = NULL,
		.term_local = NULL,
		.init_global = loop_init_global,
		.term_global = NULL,
	},
	.open = loopback_open,
	.close = loopback_close,
	.start = NULL,
	.stop = NULL,
	.stats = loopback_stats,
	.stats_reset = loopback_stats_reset,
	.pktin_ts_res = NULL,
	.pktin_ts_from_ns = NULL,
	.recv = loopback_recv,
	.send = loopback_send,
	.mtu_get = loopback_mtu_get,
	.promisc_mode_set = loopback_promisc_mode_set,
	.promisc_mode_get = loopback_promisc_mode_get,
	.mac_get = loopback_mac_addr_get,
	.mac_set = NULL,
	.link_status = loopback_link_status,
	.capability = loopback_capability,
	.config = loopback_config,
	.input_queues_config = NULL,
	.output_queues_config = NULL,
	.print = NULL,
};

ODP_MODULE_CONSTRUCTOR(loopback_pktio_ops)
{
	odp_module_constructor(&loopback_pktio_ops);

	odp_subsystem_register_module(pktio_ops, &loopback_pktio_ops);
}

/* Temporary variable to enable link this module,
 * will remove in Makefile scheme changes.
 */
int enable_link_loopback_pktio_ops = 0;
