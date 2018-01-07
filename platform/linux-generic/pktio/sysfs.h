/* Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#ifndef ODP_PKTIO_SYSFS_H_
#define ODP_PKTIO_SYSFS_H_

/**
 * Get statistics for a pktio device
 *
 * @param dev	     Pkt I/O device name
 * @param stats[out] Output buffer for counters
 *
 * @retval 0 on success
 * @retval != 0 on failure
 */

int sysfs_stats(const char *dev,
		odp_pktio_stats_t *stats);

#endif /* ODP_PKTIO_SYSFS_H_ */
