/* Copyright (c) 2017, ARM Limited. All rights reserved.
 *
 * Copyright (c) 2017, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ODP_PKTIO_OPS_LOOPBACK_H_
#define ODP_PKTIO_OPS_LOOPBACK_H_

typedef struct {
	odp_queue_t loopq;  /**< loopback queue for "loop" device */
	odp_bool_t promisc; /**< promiscuous mode state */
	odp_pktin_config_opt_t pktin_cfg; /**< pkt in config */
	odp_ticketlock_t rx_lock; /**< RX lock */
	odp_ticketlock_t tx_lock; /**< TX lock */
	odp_pktio_stats_t stats;  /**< Stats for loopback pkt IO */
} pktio_ops_loopback_data_t;

#endif
