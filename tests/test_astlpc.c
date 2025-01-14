/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "libmctp-astlpc.h"
#include "libmctp-log.h"
#include "container_of.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#define RX_BUFFER_DATA 0x100 + 4 + 4
#define TX_BUFFER_DATA 0x200

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hack: Needs to be in sync with astlpc.c */
#define binding_to_mmio(b)                                                     \
	container_of(b, struct mctp_binding_astlpc_mmio, astlpc)

uint8_t g_received_packet_count = 0;

struct mctp_binding_astlpc {
	struct mctp_binding binding;

	union {
		void *lpc_map;
		struct mctp_lpcmap_hdr *lpc_hdr;
	};

	/* direct ops data */
	struct mctp_binding_astlpc_ops ops;
	void *ops_data;
	struct mctp_lpcmap_hdr *priv_hdr;

	/* fileio ops data */
	void *lpc_map_base;
	int kcs_fd;
	uint8_t kcs_status;

	bool running;

	/* temporary transmit buffer */
	uint8_t txbuf[256];
};

#define KCS_STATUS_BMC_READY 0x80
#define KCS_STATUS_CHANNEL_ACTIVE 0x40
#define KCS_STATUS_IBF 0x02
#define KCS_STATUS_OBF 0x01

struct mctp_binding_astlpc_mmio {
	struct mctp_binding_astlpc astlpc;
	bool bmc;

	uint8_t kcs[2];

	size_t lpc_size;
	uint8_t *lpc;
};

int mctp_astlpc_mmio_kcs_read(void *data, enum mctp_binding_astlpc_kcs_reg reg,
			      uint8_t *val)
{
	struct mctp_binding_astlpc_mmio *mmio = binding_to_mmio(data);

	*val = mmio->kcs[reg];

	mctp_prdebug("%s: 0x%hhx from %s", __func__, *val,
		     reg ? "status" : "data");

	if (reg == MCTP_ASTLPC_KCS_REG_DATA) {
		uint8_t flag = mmio->bmc ? KCS_STATUS_IBF : KCS_STATUS_OBF;
		(mmio->kcs)[MCTP_ASTLPC_KCS_REG_STATUS] &= ~flag;
	}

	return 0;
}

int mctp_astlpc_mmio_kcs_write(void *data, enum mctp_binding_astlpc_kcs_reg reg,
			       uint8_t val)
{
	struct mctp_binding_astlpc_mmio *mmio = binding_to_mmio(data);

	if (reg == MCTP_ASTLPC_KCS_REG_DATA)
		mmio->kcs[MCTP_ASTLPC_KCS_REG_STATUS] |= KCS_STATUS_OBF;

	if (reg == MCTP_ASTLPC_KCS_REG_STATUS)
		mmio->kcs[reg] = val & ~0xaU;
	else if (reg == MCTP_ASTLPC_KCS_REG_DATA)
		mmio->kcs[reg] = val;

	mctp_prdebug("%s: 0x%hhx to %s", __func__, val,
		     reg ? "status" : "data");

	return 0;
}
int mctp_astlpc_mmio_lpc_read(void *data, void *buf, long offset, size_t len)
{
	struct mctp_binding_astlpc_mmio *mmio = binding_to_mmio(data);

	assert(offset >= 0L);
	assert(offset + len < mmio->lpc_size);

	memcpy(buf, mmio->lpc + offset, len);

	mctp_prdebug("%s: %zu bytes from 0x%lx", __func__, len, offset);

	return 0;
}

int mctp_astlpc_mmio_lpc_write(void *data, void *buf, long offset, size_t len)
{
	struct mctp_binding_astlpc_mmio *mmio = binding_to_mmio(data);

	assert(offset >= 0L);
	assert(offset + len < mmio->lpc_size);

	memcpy(mmio->lpc + offset, buf, len);

	mctp_prdebug("%s: %zu bytes to 0x%lx", __func__, len, offset);

	return 0;
}

static void rx_message(uint8_t eid, void *data, void *msg, size_t len,
		       bool tag_owner, uint8_t tag, void *prv)
{
	uint8_t type;

	type = *(uint8_t *)msg;

	mctp_prdebug("MCTP message received: len %zd, type %d", len, type);

	g_received_packet_count++;
}

const struct mctp_binding_astlpc_ops mctp_binding_astlpc_mmio_ops = {
	.kcs_read = mctp_astlpc_mmio_kcs_read,
	.kcs_write = mctp_astlpc_mmio_kcs_write,
	.lpc_read = mctp_astlpc_mmio_lpc_read,
	.lpc_write = mctp_astlpc_mmio_lpc_write,
};

int main(void)
{
	struct mctp_binding_astlpc_mmio mmio;
	struct mctp_binding_astlpc *astlpc;
	uint8_t msg[2 * MCTP_BTU];
	struct mctp *mctp;
	int rc;

	memset(&msg[0], 0x5a, MCTP_BTU);
	memset(&msg[MCTP_BTU], 0xa5, MCTP_BTU);

	mmio.lpc_size = 1 * 1024 * 1024;
	mmio.lpc = calloc(1, mmio.lpc_size);
	mmio.bmc = true;

	assert(mmio.lpc);

	mctp_set_log_stdio(MCTP_LOG_DEBUG);

	mctp = mctp_init();
	assert(mctp);

	mctp_set_rx_all(mctp, rx_message, NULL);

	astlpc = mctp_astlpc_init_ops(&mctp_binding_astlpc_mmio_ops, &mmio,
				      NULL);
	assert(astlpc);

	mctp_register_bus(mctp, &astlpc->binding, 8);

	/* Verify the binding was initialised */
	assert(mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] & KCS_STATUS_BMC_READY);

	/* Host sends channel init command */
	mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] |= KCS_STATUS_IBF;
	mmio.kcs[MCTP_ASTLPC_KCS_REG_DATA] = 0x00;

	/* Receive host init */
	mctp_astlpc_poll(astlpc);

	/* Host receives init response */
	assert(mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] & KCS_STATUS_OBF);
	assert(mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] &
	       KCS_STATUS_CHANNEL_ACTIVE);

	/* Host dequeues data */
	assert(mmio.kcs[MCTP_ASTLPC_KCS_REG_DATA] == 0xff);
	mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] &= ~KCS_STATUS_OBF;

	/* BMC sends a message */
	rc = mctp_message_tx(mctp, 9, msg, sizeof(msg), true, 0, NULL);
	assert(rc == 0 || rc == TX_DISABLED_ERR);

	/* Host receives a message */
	assert(mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] & KCS_STATUS_OBF);
	assert(mmio.kcs[MCTP_ASTLPC_KCS_REG_DATA] == 0x01);

	/* Verify it's the packet we expect */
	assert(!memcmp(mmio.lpc + RX_BUFFER_DATA, &msg[0], MCTP_BTU));

	/* Host returns Rx area ownership to BMC */
	mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] &= ~KCS_STATUS_OBF;
	mmio.kcs[MCTP_ASTLPC_KCS_REG_DATA] = 0x02;
	mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] |= KCS_STATUS_IBF;

	/* BMC dequeues ownership hand-over and sends the queued packet */
	rc = mctp_astlpc_poll(astlpc);
	assert(rc == 0);

	/* Host receives a message */
	assert(mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] & KCS_STATUS_OBF);
	assert(mmio.kcs[MCTP_ASTLPC_KCS_REG_DATA] == 0x01);

	/* Verify it's the packet we expect */
	assert(!memcmp(mmio.lpc + RX_BUFFER_DATA, &msg[MCTP_BTU], MCTP_BTU));

	mctp_prdebug("Host sends dummy packets..");
	mmio.bmc = false;

	uint8_t valid_packet[] = { 0x00, 0x00, 0x00, 0x07, 0x01, 0x08,
				   0x09, 0xC8, 0x00, 0x81, 0x02 };
	memset(mmio.lpc + TX_BUFFER_DATA, 0, sizeof(valid_packet));
	memcpy(mmio.lpc + TX_BUFFER_DATA, &valid_packet[0],
	       sizeof(valid_packet));

	mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] |= KCS_STATUS_IBF;
	mmio.kcs[MCTP_ASTLPC_KCS_REG_DATA] = 0x01;
	rc = mctp_astlpc_poll(astlpc);
	assert(rc == 0);
	assert(g_received_packet_count == 1);

	uint8_t invalid_packet[] = { 0x00, 0x00, 0x00, 0x03, 0x01, 0x08, 0x09 };
	memset(mmio.lpc + TX_BUFFER_DATA, 0, sizeof(invalid_packet));
	memcpy(mmio.lpc + TX_BUFFER_DATA, &invalid_packet[0],
	       sizeof(invalid_packet));

	mmio.kcs[MCTP_ASTLPC_KCS_REG_STATUS] |= KCS_STATUS_IBF;
	mmio.kcs[MCTP_ASTLPC_KCS_REG_DATA] = 0x01;
	rc = mctp_astlpc_poll(astlpc);
	assert(rc == 0);
	/* Since we transmitted a 3 byte packet to BMC, BMC should discard
         * the packet since size is less than MCTP header. Confirm using the
         * received packet counter which get incremented only on reception of
         * valid MCTP message */
	assert(g_received_packet_count == 1);

	mctp_astlpc_destroy(astlpc);
	mctp_destroy(mctp);
	free(mmio.lpc);

	return 0;
}
