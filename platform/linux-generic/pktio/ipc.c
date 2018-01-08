/* Copyright (c) 2015, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include "config.h"

#ifdef ODP_PKTIO_IPC

#include <odp_debug_internal.h>
#include <odp_packet_io_internal.h>
#include <odp/api/system_info.h>
#include <odp_shm_internal.h>
#include <_ishm_internal.h>
#include <odp_pktio_ops_ipc.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/if_ether.h>

#define IPC_ODP_DEBUG_PRINT 0

#define IPC_ODP_DBG(fmt, ...) \
	do { \
		if (IPC_ODP_DEBUG_PRINT == 1) \
			ODP_DBG(fmt, ##__VA_ARGS__);\
	} while (0)

/* MAC address for the "ipc" interface */
static const char pktio_ipc_mac[] = {0x12, 0x12, 0x12, 0x12, 0x12, 0x12};

static odp_shm_t _ipc_map_remote_pool(const char *name, int pid);

static const char *_ipc_odp_buffer_pool_shm_name(odp_pool_t pool_hdl)
{
	pool_t *pool;
	odp_shm_t shm;
	odp_shm_info_t info;

	pool    = pool_entry_from_hdl(pool_hdl);
	shm = pool->shm;

	odp_shm_info(shm, &info);

	return info.name;
}

static int _ipc_master_start(pktio_entry_t *pktio_entry)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;
	struct pktio_info *pinfo = pkt_ipc->pinfo;
	odp_shm_t shm;

	if (pinfo->slave.init_done == 0)
		return -1;

	shm = _ipc_map_remote_pool(pinfo->slave.pool_name,
				   pinfo->slave.pid);
	if (shm == ODP_SHM_INVALID) {
		ODP_DBG("no pool file %s for pid %d\n",
			pinfo->slave.pool_name, pinfo->slave.pid);
		return -1;
	}

	pkt_ipc->remote_pool_shm = shm;
	pkt_ipc->pool_base = odp_shm_addr(shm);
	pkt_ipc->pool_mdata_base = (char *)odp_shm_addr(shm);

	odp_atomic_store_u32(&pkt_ipc->ready, 1);

	IPC_ODP_DBG("%s started.\n",  pktio_entry->s.name);
	return 0;
}

static int _ipc_init_master(pktio_entry_t *pktio_entry,
			    const char *dev,
			    odp_pool_t pool_hdl)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;
	char ipc_shm_name[ODP_POOL_NAME_LEN + sizeof("_m_prod")];
	pool_t *pool;
	struct pktio_info *pinfo;
	const char *pool_name;

	pool = pool_entry_from_hdl(pool_hdl);
	(void)pool;

	if (strlen(dev) > (ODP_POOL_NAME_LEN - sizeof("_m_prod"))) {
		ODP_ERR("too big ipc name\n");
		return -1;
	}

	/* generate name in shm like ipc_pktio_r for
	 * to be processed packets ring.
	 */
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_prod", dev);
	pkt_ipc->tx.send = _ring_create(ipc_shm_name,
			PKTIO_IPC_ENTRIES,
			_RING_SHM_PROC | _RING_NO_LIST);
	if (!pkt_ipc->tx.send) {
		ODP_ERR("pid %d unable to create ipc ring %s name\n",
			getpid(), ipc_shm_name);
		return -1;
	}
	ODP_DBG("Created IPC ring: %s, count %d, free %d\n",
		ipc_shm_name, _ring_count(pkt_ipc->tx.send),
		_ring_free_count(pkt_ipc->tx.send));

	/* generate name in shm like ipc_pktio_p for
	 * already processed packets
	 */
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_cons", dev);
	pkt_ipc->tx.free = _ring_create(ipc_shm_name,
			PKTIO_IPC_ENTRIES,
			_RING_SHM_PROC | _RING_NO_LIST);
	if (!pkt_ipc->tx.free) {
		ODP_ERR("pid %d unable to create ipc ring %s name\n",
			getpid(), ipc_shm_name);
		goto free_m_prod;
	}
	ODP_DBG("Created IPC ring: %s, count %d, free %d\n",
		ipc_shm_name, _ring_count(pkt_ipc->tx.free),
		_ring_free_count(pkt_ipc->tx.free));

	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_s_prod", dev);
	pkt_ipc->rx.recv = _ring_create(ipc_shm_name,
			PKTIO_IPC_ENTRIES,
			_RING_SHM_PROC | _RING_NO_LIST);
	if (!pkt_ipc->rx.recv) {
		ODP_ERR("pid %d unable to create ipc ring %s name\n",
			getpid(), ipc_shm_name);
		goto free_m_cons;
	}
	ODP_DBG("Created IPC ring: %s, count %d, free %d\n",
		ipc_shm_name, _ring_count(pkt_ipc->rx.recv),
		_ring_free_count(pkt_ipc->rx.recv));

	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_s_cons", dev);
	pkt_ipc->rx.free = _ring_create(ipc_shm_name,
			PKTIO_IPC_ENTRIES,
			_RING_SHM_PROC | _RING_NO_LIST);
	if (!pkt_ipc->rx.free) {
		ODP_ERR("pid %d unable to create ipc ring %s name\n",
			getpid(), ipc_shm_name);
		goto free_s_prod;
	}
	ODP_DBG("Created IPC ring: %s, count %d, free %d\n",
		ipc_shm_name, _ring_count(pkt_ipc->rx.free),
		_ring_free_count(pkt_ipc->rx.free));

	/* Set up pool name for remote info */
	pinfo = pkt_ipc->pinfo;
	pool_name = _ipc_odp_buffer_pool_shm_name(pool_hdl);
	if (strlen(pool_name) > ODP_POOL_NAME_LEN) {
		ODP_ERR("pid %d ipc pool name %s is too big %d\n",
			getpid(), pool_name, strlen(pool_name));
		goto free_s_prod;
	}

	memcpy(pinfo->master.pool_name, pool_name, strlen(pool_name));
	pinfo->slave.base_addr = 0;
	pinfo->slave.pid = 0;
	pinfo->slave.init_done = 0;

	pkt_ipc->pool = pool_hdl;

	ODP_DBG("Pre init... DONE.\n");
	pinfo->master.init_done = 1;

	_ipc_master_start(pktio_entry);

	return 0;

free_s_prod:
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_s_prod", dev);
	_ring_destroy(ipc_shm_name);
free_m_cons:
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_cons", dev);
	_ring_destroy(ipc_shm_name);
free_m_prod:
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_prod", dev);
	_ring_destroy(ipc_shm_name);
	return -1;
}

static void _ipc_export_pool(struct pktio_info *pinfo,
			     odp_pool_t pool_hdl)
{
	pool_t *pool = pool_entry_from_hdl(pool_hdl);

	snprintf(pinfo->slave.pool_name, ODP_POOL_NAME_LEN, "%s",
		 _ipc_odp_buffer_pool_shm_name(pool_hdl));
	pinfo->slave.pid = odp_global_data.main_pid;
	pinfo->slave.block_size = pool->block_size;
	pinfo->slave.base_addr = pool->base_addr;
}

static odp_shm_t _ipc_map_remote_pool(const char *name, int pid)
{
	odp_shm_t shm;
	char rname[ODP_SHM_NAME_LEN];

	snprintf(rname, ODP_SHM_NAME_LEN, "remote-%s", name);
	shm = odp_shm_import(name, pid, rname);
	if (shm == ODP_SHM_INVALID) {
		ODP_ERR("unable map %s\n", name);
		return ODP_SHM_INVALID;
	}

	IPC_ODP_DBG("Mapped remote pool %s to local %s\n", name, rname);
	return shm;
}

static void *_ipc_shm_map(char *name, int pid)
{
	odp_shm_t shm;

	shm = odp_shm_import(name, pid, name);
	if (ODP_SHM_INVALID == shm) {
		ODP_ERR("unable to map: %s\n", name);
		return NULL;
	}

	return odp_shm_addr(shm);
}

static int _ipc_init_slave(const char *dev,
			   pktio_entry_t *pktio_entry,
			   odp_pool_t pool)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;

	if (strlen(dev) > (ODP_POOL_NAME_LEN - sizeof("_slave_r")))
		ODP_ABORT("too big ipc name\n");

	pkt_ipc->pool = pool;
	return 0;
}

static int _ipc_slave_start(pktio_entry_t *pktio_entry)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;
	char ipc_shm_name[ODP_POOL_NAME_LEN + sizeof("_slave_r")];
	struct pktio_info *pinfo;
	odp_shm_t shm;
	char tail[ODP_POOL_NAME_LEN];
	char dev[ODP_POOL_NAME_LEN];
	int pid;

	if (sscanf(pktio_entry->s.name, "ipc:%d:%s", &pid, tail) != 2) {
		ODP_ERR("wrong pktio name\n");
		return -1;
	}

	sprintf(dev, "ipc:%s", tail);

	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_prod", dev);
	pkt_ipc->rx.recv  = _ipc_shm_map(ipc_shm_name, pid);
	if (!pkt_ipc->rx.recv) {
		ODP_DBG("pid %d unable to find ipc ring %s name\n",
			getpid(), dev);
		sleep(1);
		return -1;
	}
	ODP_DBG("Connected IPC ring: %s, count %d, free %d\n",
		ipc_shm_name, _ring_count(pkt_ipc->rx.recv),
		_ring_free_count(pkt_ipc->rx.recv));

	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_cons", dev);
	pkt_ipc->rx.free = _ipc_shm_map(ipc_shm_name, pid);
	if (!pkt_ipc->rx.free) {
		ODP_ERR("pid %d unable to find ipc ring %s name\n",
			getpid(), dev);
		goto free_m_prod;
	}
	ODP_DBG("Connected IPC ring: %s, count %d, free %d\n",
		ipc_shm_name, _ring_count(pkt_ipc->rx.free),
		_ring_free_count(pkt_ipc->rx.free));

	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_s_prod", dev);
	pkt_ipc->tx.send = _ipc_shm_map(ipc_shm_name, pid);
	if (!pkt_ipc->tx.send) {
		ODP_ERR("pid %d unable to find ipc ring %s name\n",
			getpid(), dev);
		goto free_m_cons;
	}
	ODP_DBG("Connected IPC ring: %s, count %d, free %d\n",
		ipc_shm_name, _ring_count(pkt_ipc->tx.send),
		_ring_free_count(pkt_ipc->tx.send));

	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_s_cons", dev);
	pkt_ipc->tx.free = _ipc_shm_map(ipc_shm_name, pid);
	if (!pkt_ipc->tx.free) {
		ODP_ERR("pid %d unable to find ipc ring %s name\n",
			getpid(), dev);
		goto free_s_prod;
	}
	ODP_DBG("Connected IPC ring: %s, count %d, free %d\n",
		ipc_shm_name, _ring_count(pkt_ipc->tx.free),
		_ring_free_count(pkt_ipc->tx.free));

	/* Get info about remote pool */
	pinfo = pkt_ipc->pinfo;
	shm = _ipc_map_remote_pool(pinfo->master.pool_name,
				   pid);
	pkt_ipc->remote_pool_shm = shm;
	pkt_ipc->pool_mdata_base = (char *)odp_shm_addr(shm);
	pkt_ipc->pkt_size = pinfo->master.block_size;

	_ipc_export_pool(pinfo, pkt_ipc->pool);

	odp_atomic_store_u32(&pkt_ipc->ready, 1);
	pinfo->slave.init_done = 1;

	ODP_DBG("%s started.\n",  pktio_entry->s.name);
	return 0;

free_s_prod:
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_s_prod", dev);
	shm = odp_shm_lookup(ipc_shm_name);
	odp_shm_free(shm);
free_m_cons:
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_cons", dev);
	shm = odp_shm_lookup(ipc_shm_name);
	odp_shm_free(shm);
free_m_prod:
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_prod", dev);
	shm = odp_shm_lookup(ipc_shm_name);
	odp_shm_free(shm);
	return -1;
}

static int ipc_pktio_open(odp_pktio_t id ODP_UNUSED,
			  pktio_entry_t *pktio_entry,
			  const char *dev,
			  odp_pool_t pool)
{
	int ret = -1;
	int pid ODP_UNUSED;
	struct pktio_info *pinfo;
	char name[ODP_POOL_NAME_LEN + sizeof("_info")];
	char tail[ODP_POOL_NAME_LEN];
	odp_shm_t shm;
	pktio_ops_ipc_data_t *pkt_ipc;

	ODP_STATIC_ASSERT(ODP_POOL_NAME_LEN == _RING_NAMESIZE,
			  "mismatch pool and ring name arrays");

	if (strncmp(dev, "ipc", 3))
		return -1;

	pktio_entry->s.ops_data = ODP_OPS_DATA_ALLOC(sizeof(*pkt_ipc));
	if (odp_unlikely(pktio_entry->s.ops_data == NULL)) {
		ODP_ERR("Failed to allocate pktio_ops_ipc_data_t struct");
		return -1;
	}

	pkt_ipc = pktio_entry->s.ops_data;
	memset(pkt_ipc, 0, sizeof(*pkt_ipc));

	odp_atomic_init_u32(&pkt_ipc->ready, 0);

	pkt_ipc->rx.cache = _ring_create("ipc_rx_cache",
						   PKTIO_IPC_ENTRIES,
						   _RING_NO_LIST);

	/* Shared info about remote pktio */
	if (sscanf(dev, "ipc:%d:%s", &pid, tail) == 2) {
		pkt_ipc->type = PKTIO_TYPE_IPC_SLAVE;

		snprintf(name, sizeof(name), "ipc:%s_info", tail);
		IPC_ODP_DBG("lookup for name %s for pid %d\n", name, pid);
		shm = odp_shm_import(name, pid, name);
		if (ODP_SHM_INVALID == shm) {
			ODP_OPS_DATA_FREE(pktio_entry->s.ops_data);
			return -1;
		}
		pinfo = odp_shm_addr(shm);

		if (!pinfo->master.init_done) {
			odp_shm_free(shm);
			ODP_OPS_DATA_FREE(pktio_entry->s.ops_data);
			return -1;
		}
		pkt_ipc->pinfo = pinfo;
		pkt_ipc->pinfo_shm = shm;
		ODP_DBG("process %d is slave\n", getpid());
		ret = _ipc_init_slave(name, pktio_entry, pool);
	} else {
		pkt_ipc->type = PKTIO_TYPE_IPC_MASTER;
		snprintf(name, sizeof(name), "%s_info", dev);
		shm = odp_shm_reserve(name, sizeof(struct pktio_info),
				      ODP_CACHE_LINE_SIZE,
				      _ODP_ISHM_EXPORT | _ODP_ISHM_LOCK);
		if (ODP_SHM_INVALID == shm) {
			ODP_ERR("can not create shm %s\n", name);
			ODP_OPS_DATA_FREE(pktio_entry->s.ops_data);
			return -1;
		}

		pinfo = odp_shm_addr(shm);
		pinfo->master.init_done = 0;
		pinfo->master.pool_name[0] = 0;
		pkt_ipc->pinfo = pinfo;
		pkt_ipc->pinfo_shm = shm;
		ODP_DBG("process %d is master\n", getpid());
		ret = _ipc_init_master(pktio_entry, dev, pool);
	}

	if (ret)
		ODP_OPS_DATA_FREE(pktio_entry->s.ops_data);
	return ret;
}

static void _ipc_free_ring_packets(pktio_entry_t *pktio_entry, _ring_t *r)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;
	uintptr_t offsets[PKTIO_IPC_ENTRIES];
	int ret;
	void **rbuf_p;
	int i;
	void *addr;
	pool_t *pool;

	if (!r)
		return;

	pool = pool_entry_from_hdl(pkt_ipc->pool);
	addr = odp_shm_addr(pool->shm);

	rbuf_p = (void *)&offsets;

	while (1) {
		ret = _ring_mc_dequeue_burst(r, rbuf_p,
					     PKTIO_IPC_ENTRIES);
		if (ret <= 0)
			break;
		for (i = 0; i < ret; i++) {
			odp_packet_hdr_t *phdr;
			odp_packet_t pkt;

			phdr = (void *)((uint8_t *)addr + offsets[i]);
			pkt = packet_handle(phdr);

			odp_packet_free(pkt);
		}
	}
}

static int ipc_pktio_recv_lockless(pktio_entry_t *pktio_entry,
				   odp_packet_t pkt_table[], int len)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;
	int pkts = 0;
	int i;
	_ring_t *r;
	_ring_t *r_p;
	uintptr_t offsets[PKTIO_IPC_ENTRIES];
	void **ipcbufs_p = (void *)&offsets[0];
	uint32_t ready;
	int pkts_ring;

	ready = odp_atomic_load_u32(&pkt_ipc->ready);
	if (odp_unlikely(!ready)) {
		IPC_ODP_DBG("start pktio is missing before usage?\n");
		return 0;
	}

	_ipc_free_ring_packets(pktio_entry, pkt_ipc->tx.free);

	/* rx from cache */
	r = pkt_ipc->rx.cache;
	pkts = _ring_mc_dequeue_burst(r, ipcbufs_p, len);
	if (odp_unlikely(pkts < 0))
		ODP_ABORT("internal error dequeue\n");

	/* rx from other app */
	if (pkts == 0) {
		ipcbufs_p = (void *)&offsets[0];
		r = pkt_ipc->rx.recv;
		pkts = _ring_mc_dequeue_burst(r, ipcbufs_p, len);
		if (odp_unlikely(pkts < 0))
			ODP_ABORT("internal error dequeue\n");
	}

	/* fast path */
	if (odp_likely(0 == pkts))
		return 0;

	for (i = 0; i < pkts; i++) {
		odp_pool_t pool;
		odp_packet_t pkt;
		odp_packet_hdr_t *phdr;
		void *pkt_data;
		uint64_t data_pool_off;
		void *rmt_data_ptr;

		phdr = (void *)((uint8_t *)pkt_ipc->pool_mdata_base +
			offsets[i]);

		pool = pkt_ipc->pool;
		if (odp_unlikely(pool == ODP_POOL_INVALID))
			ODP_ABORT("invalid pool");

		data_pool_off = phdr->buf_hdr.ipc_data_offset;

		pkt = odp_packet_alloc(pool, phdr->frame_len);
		if (odp_unlikely(pkt == ODP_PACKET_INVALID)) {
			/* Original pool might be smaller then
			*  PKTIO_IPC_ENTRIES. If packet can not be
			 * allocated from pool at this time,
			 * simple get in on next recv() call. To keep
			 * packet ordering store such packets in local
			 * cache.
			 */
			IPC_ODP_DBG("unable to allocate packet %d/%d\n",
				    i, pkts);
			break;
		}

		/* Copy packet data. */
		pkt_data = odp_packet_data(pkt);
		if (odp_unlikely(!pkt_data))
			ODP_ABORT("unable to map pkt_data ipc_slave %d\n",
				  (PKTIO_TYPE_IPC_SLAVE == pkt_ipc->type));

		/* Copy packet data from shared pool to local pool. */
		rmt_data_ptr = (uint8_t *)pkt_ipc->pool_mdata_base +
			data_pool_off;
		memcpy(pkt_data, rmt_data_ptr, phdr->frame_len);

		odp_packet_hdr(pkt)->frame_len = phdr->frame_len;
		odp_packet_hdr(pkt)->headroom = phdr->headroom;
		odp_packet_hdr(pkt)->tailroom = phdr->tailroom;

		/* Take classification fields */
		odp_packet_hdr(pkt)->p = phdr->p;

		pkt_table[i] = pkt;
	}

	/* put back to rx ring dequed but not processed packets*/
	if (pkts != i) {
		ipcbufs_p = (void *)&offsets[i];
		r_p = pkt_ipc->rx.cache;
		pkts_ring = _ring_mp_enqueue_burst(r_p, ipcbufs_p, pkts - i);

		if (pkts_ring != (pkts - i))
			ODP_ABORT("bug to enqueue packets\n");

		if (i == 0)
			return 0;
	}

	/*num of actually received packets*/
	pkts = i;

	/* Now tell other process that we no longer need that buffers.*/
	r_p = pkt_ipc->rx.free;

repeat:

	ipcbufs_p = (void *)&offsets[0];
	pkts_ring = _ring_mp_enqueue_burst(r_p, ipcbufs_p, pkts);
	if (odp_unlikely(pkts_ring < 0))
		ODP_ABORT("ipc: odp_ring_mp_enqueue_bulk r_p fail\n");

	for (i = 0; i < pkts; i++) {
		IPC_ODP_DBG("%d/%d send to be free packet offset %x\n",
			    i, pkts, offsets[i]);
	}

	if (odp_unlikely(pkts != pkts_ring)) {
		IPC_ODP_DBG("odp_ring_full: %d, odp_ring_count %d,"
			    " _ring_free_count %d\n",
			    _ring_full(r_p), _ring_count(r_p),
			    _ring_free_count(r_p));
		ipcbufs_p = (void *)&offsets[pkts_ring - 1];
		pkts = pkts - pkts_ring;
		goto repeat;
	}

	return pkts;
}

static int ipc_pktio_recv(pktio_entry_t *pktio_entry, int index ODP_UNUSED,
			  odp_packet_t pkt_table[], int len)
{
	int ret;

	odp_ticketlock_lock(&pktio_entry->s.rxl);

	ret = ipc_pktio_recv_lockless(pktio_entry, pkt_table, len);

	odp_ticketlock_unlock(&pktio_entry->s.rxl);

	return ret;
}

static int ipc_pktio_send_lockless(pktio_entry_t *pktio_entry,
				   const odp_packet_t pkt_table[], int len)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;
	_ring_t *r;
	void **rbuf_p;
	int ret;
	int i;
	uint32_t ready = odp_atomic_load_u32(&pkt_ipc->ready);
	odp_packet_t pkt_table_mapped[len]; /**< Ready to send packet has to be
					      * in memory mapped pool. */
	uintptr_t offsets[len];

	if (odp_unlikely(!ready))
		return 0;

	_ipc_free_ring_packets(pktio_entry, pkt_ipc->tx.free);

	/* Copy packets to shm shared pool if they are in different
	 * pool, or if they are references (we can't share across IPC).
	 */
	for (i = 0; i < len; i++) {
		odp_packet_t pkt =  pkt_table[i];
		pool_t *ipc_pool = pool_entry_from_hdl(
			pkt_ipc->pool);
		odp_packet_hdr_t *pkt_hdr;
		pool_t *pool;

		pkt_hdr = odp_packet_hdr(pkt);
		pool = pkt_hdr->buf_hdr.pool_ptr;

		if (pool->pool_idx != ipc_pool->pool_idx ||
		    odp_packet_has_ref(pkt)) {
			odp_packet_t newpkt;

			newpkt = odp_packet_copy(pkt, pkt_ipc->pool);
			if (newpkt == ODP_PACKET_INVALID)
				ODP_ABORT("Unable to copy packet\n");

			odp_packet_free(pkt);
			pkt_table_mapped[i] = newpkt;
		} else {
			pkt_table_mapped[i] = pkt;
		}
	}

	/* Set offset to phdr for outgoing packets */
	for (i = 0; i < len; i++) {
		uint64_t data_pool_off;
		odp_packet_t pkt = pkt_table_mapped[i];
		odp_packet_hdr_t *pkt_hdr = odp_packet_hdr(pkt);
		odp_pool_t pool_hdl = odp_packet_pool(pkt);
		pool_t *pool = pool_entry_from_hdl(pool_hdl);

		offsets[i] = (uint8_t *)pkt_hdr -
			     (uint8_t *)odp_shm_addr(pool->shm);
		data_pool_off = (uint8_t *)pkt_hdr->buf_hdr.seg[0].data -
				(uint8_t *)odp_shm_addr(pool->shm);

		/* compile all function code even if ipc disabled with config */
		pkt_hdr->buf_hdr.ipc_data_offset = data_pool_off;
		IPC_ODP_DBG("%d/%d send packet %llx, pool %llx,"
			    "phdr = %p, offset %x sendoff %x, addr %llx iaddr %llx\n",
			    i, len,
			    odp_packet_to_u64(pkt), odp_pool_to_u64(pool_hdl),
			    pkt_hdr, pkt_hdr->buf_hdr.ipc_data_offset,
			    offsets[i], odp_shm_addr(pool->shm),
			    odp_shm_addr(pool_entry_from_hdl(
					 pkt_ipc->pool)->shm));
	}

	/* Put packets to ring to be processed by other process. */
	rbuf_p = (void *)&offsets[0];
	r = pkt_ipc->tx.send;
	ret = _ring_mp_enqueue_burst(r, rbuf_p, len);
	if (odp_unlikely(ret < 0)) {
		ODP_ERR("pid %d odp_ring_mp_enqueue_bulk fail, ipc_slave %d, ret %d\n",
			getpid(), (PKTIO_TYPE_IPC_SLAVE == pkt_ipc->type), ret);
		ODP_ERR("odp_ring_full: %d, odp_ring_count %d, _ring_free_count %d\n",
			_ring_full(r), _ring_count(r),
			_ring_free_count(r));
		ODP_ABORT("Unexpected!\n");
	}

	return len;
}

static int ipc_pktio_send(pktio_entry_t *pktio_entry, int index ODP_UNUSED,
			  const odp_packet_t pkt_table[], int len)
{
	int ret;

	odp_ticketlock_lock(&pktio_entry->s.txl);

	ret = ipc_pktio_send_lockless(pktio_entry, pkt_table, len);

	odp_ticketlock_unlock(&pktio_entry->s.txl);

	return ret;
}

static uint32_t ipc_mtu_get(pktio_entry_t *pktio_entry ODP_UNUSED)
{
	/* mtu not limited, pool settings are used. */
	return (9 * 1024);
}

static int ipc_mac_addr_get(pktio_entry_t *pktio_entry ODP_UNUSED,
			    void *mac_addr)
{
	memcpy(mac_addr, pktio_ipc_mac, ETH_ALEN);
	return ETH_ALEN;
}

static int ipc_start(pktio_entry_t *pktio_entry)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;
	uint32_t ready = odp_atomic_load_u32(&pkt_ipc->ready);

	if (ready) {
		ODP_ABORT("%s Already started\n", pktio_entry->s.name);
		return -1;
	}

	if (pkt_ipc->type == PKTIO_TYPE_IPC_MASTER)
		return _ipc_master_start(pktio_entry);
	else
		return _ipc_slave_start(pktio_entry);
}

static int ipc_stop(pktio_entry_t *pktio_entry)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;
	unsigned tx_send = 0, tx_free = 0;

	odp_atomic_store_u32(&pkt_ipc->ready, 0);

	if (pkt_ipc->tx.send)
		_ipc_free_ring_packets(pktio_entry, pkt_ipc->tx.send);
	/* other process can transfer packets from one ring to
	 * other, use delay here to free that packets. */
	sleep(1);
	if (pkt_ipc->tx.free)
		_ipc_free_ring_packets(pktio_entry, pkt_ipc->tx.free);

	if (pkt_ipc->tx.send)
		tx_send = _ring_count(pkt_ipc->tx.send);
	if (pkt_ipc->tx.free)
		tx_free = _ring_count(pkt_ipc->tx.free);
	if (tx_send | tx_free) {
		ODP_DBG("IPC rings: tx send %d tx free %d\n",
			tx_send, tx_free);
	}

	return 0;
}

static int ipc_close(pktio_entry_t *pktio_entry)
{
	pktio_ops_ipc_data_t *pkt_ipc = pktio_entry->s.ops_data;
	char ipc_shm_name[ODP_POOL_NAME_LEN + sizeof("_m_prod")];
	char *dev = pktio_entry->s.name;
	char name[ODP_POOL_NAME_LEN];
	char tail[ODP_POOL_NAME_LEN];
	int pid = 0;

	ipc_stop(pktio_entry);

	odp_shm_free(pkt_ipc->remote_pool_shm);

	if (sscanf(dev, "ipc:%d:%s", &pid, tail) == 2)
		snprintf(name, sizeof(name), "ipc:%s", tail);
	else
		snprintf(name, sizeof(name), "%s", dev);

	/* unlink this pktio info for both master and slave */
	odp_shm_free(pkt_ipc->pinfo_shm);

	/* destroy rings */
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_s_cons", name);
	_ring_destroy(ipc_shm_name);
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_s_prod", name);
	_ring_destroy(ipc_shm_name);
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_cons", name);
	_ring_destroy(ipc_shm_name);
	snprintf(ipc_shm_name, sizeof(ipc_shm_name), "%s_m_prod", name);
	_ring_destroy(ipc_shm_name);
	_ring_destroy("ipc_rx_cache");

	return ODP_OPS_DATA_FREE(pktio_entry->s.ops_data);
}

static int ipc_pktio_init_global(void)
{
	_ring_tailq_init();
	ODP_PRINT("PKTIO: initialized ipc interface.\n");
	return 0;
}

static pktio_ops_module_t ipc_pktio_ops = {
	.base = {
		.name = "ipc",
		.init_local = NULL,
		.term_local = NULL,
		.init_global = ipc_pktio_init_global,
		.term_global = NULL,
	},
	.open = ipc_pktio_open,
	.close = ipc_close,
	.start = ipc_start,
	.stop = ipc_stop,
	.stats = NULL,
	.stats_reset = NULL,
	.pktin_ts_res = NULL,
	.pktin_ts_from_ns = NULL,
	.recv = ipc_pktio_recv,
	.send = ipc_pktio_send,
	.mtu_get = ipc_mtu_get,
	.promisc_mode_set = NULL,
	.promisc_mode_get = NULL,
	.mac_get = ipc_mac_addr_get,
	.mac_set = NULL,
	.link_status = NULL,
	.capability = NULL,
	.config = NULL,
	.input_queues_config = NULL,
	.output_queues_config = NULL,
	.print = NULL,
};

ODP_MODULE_CONSTRUCTOR(ipc_pktio_ops)
{
	odp_module_constructor(&ipc_pktio_ops);

	odp_subsystem_register_module(pktio_ops, &ipc_pktio_ops);
}

/* Temporary variable to enable link this module,
 * will remove in Makefile scheme changes.
 */
int enable_link_ipc_pktio_ops = 0;

#endif /* ODP_PKTIO_IPC */
