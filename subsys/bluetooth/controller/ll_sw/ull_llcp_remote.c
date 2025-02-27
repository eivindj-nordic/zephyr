/*
 * Copyright (c) 2020 Demant
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>

#include <bluetooth/hci.h>
#include <sys/byteorder.h>
#include <sys/slist.h>
#include <sys/util.h>

#include "hal/ccm.h"

#include "util/util.h"
#include "util/mem.h"
#include "util/memq.h"
#include "util/dbuf.h"

#include "pdu.h"
#include "ll.h"
#include "ll_settings.h"

#include "lll.h"
#include "lll/lll_df_types.h"
#include "lll_conn.h"

#include "ull_tx_queue.h"

#include "ull_conn_types.h"
#include "ull_conn_internal.h"
#include "ull_llcp.h"
#include "ull_llcp_internal.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_DEBUG_HCI_DRIVER)
#define LOG_MODULE_NAME bt_ctlr_ull_llcp_remote
#include "common/log.h"
#include <soc.h>
#include "hal/debug.h"

static void rr_check_done(struct ll_conn *conn, struct proc_ctx *ctx);
static struct proc_ctx *rr_dequeue(struct ll_conn *conn);
static void rr_abort(struct ll_conn *conn);

/* LLCP Remote Request FSM State */
enum rr_state {
	RR_STATE_IDLE,
	RR_STATE_REJECT,
	RR_STATE_UNSUPPORTED,
	RR_STATE_ACTIVE,
	RR_STATE_DISCONNECT,
	RR_STATE_TERMINATE,
};

/* LLCP Remote Request FSM Event */
enum {
	/* Procedure prepare */
	RR_EVT_PREPARE,

	/* Procedure run */
	RR_EVT_RUN,

	/* Procedure completed */
	RR_EVT_COMPLETE,

	/* Link connected */
	RR_EVT_CONNECT,

	/* Link disconnected */
	RR_EVT_DISCONNECT,
};

static bool proc_with_instant(struct proc_ctx *ctx)
{
	/*
	 * TODO: should we combine all the cases that return 0
	 * and all the cases that return 1?
	 */
	switch (ctx->proc) {
	case PROC_UNKNOWN:
		return 0U;
	case PROC_FEATURE_EXCHANGE:
		return 0U;
	case PROC_MIN_USED_CHANS:
		return 0U;
	case PROC_LE_PING:
		return 0U;
	case PROC_VERSION_EXCHANGE:
		return 0U;
	case PROC_ENCRYPTION_START:
	case PROC_ENCRYPTION_PAUSE:
		return 0U;
	case PROC_PHY_UPDATE:
		return 1U;
	case PROC_CONN_UPDATE:
	case PROC_CONN_PARAM_REQ:
		return 1U;
	case PROC_TERMINATE:
		return 0U;
	case PROC_CHAN_MAP_UPDATE:
		return 1U;
	case PROC_DATA_LENGTH_UPDATE:
		return 0U;
	case PROC_CTE_REQ:
		return 0U;
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
		break;
	}

	return 0U;
}

static void rr_check_done(struct ll_conn *conn, struct proc_ctx *ctx)
{
	if (ctx->done) {
		struct proc_ctx *ctx_header;

		ctx_header = llcp_rr_peek(conn);
		LL_ASSERT(ctx_header == ctx);

		rr_dequeue(conn);
		llcp_proc_ctx_release(ctx);
	}
}
/*
 * LLCP Remote Request FSM
 */

static void rr_set_state(struct ll_conn *conn, enum rr_state state)
{
	conn->llcp.remote.state = state;
}

void llcp_rr_set_incompat(struct ll_conn *conn, enum proc_incompat incompat)
{
	conn->llcp.remote.incompat = incompat;
}

void llcp_rr_set_paused_cmd(struct ll_conn *conn, enum llcp_proc proc)
{
#if defined(CONFIG_BT_CTLR_DF_CONN_CTE_RSP) || defined(CONFIG_BT_CTLR_DF_CONN_CTE_REQ)
	conn->llcp.remote.paused_cmd = proc;
#endif /* CONFIG_BT_CTLR_DF_CONN_CTE_RSP || CONFIG_BT_CTLR_DF_CONN_CTE_REQ */
}

enum llcp_proc llcp_rr_get_paused_cmd(struct ll_conn *conn)
{
#if defined(CONFIG_BT_CTLR_DF_CONN_CTE_RSP) || defined(CONFIG_BT_CTLR_DF_CONN_CTE_REQ)
	return conn->llcp.remote.paused_cmd;
#else
	return PROC_NONE;
#endif /* CONFIG_BT_CTLR_DF_CONN_CTE_RSP || CONFIG_BT_CTLR_DF_CONN_CTE_REQ */
}

static enum proc_incompat rr_get_incompat(struct ll_conn *conn)
{
	return conn->llcp.remote.incompat;
}

static void rr_set_collision(struct ll_conn *conn, bool collision)
{
	conn->llcp.remote.collision = collision;
}

bool llcp_rr_get_collision(struct ll_conn *conn)
{
	return conn->llcp.remote.collision;
}

static void rr_enqueue(struct ll_conn *conn, struct proc_ctx *ctx)
{
	sys_slist_append(&conn->llcp.remote.pend_proc_list, &ctx->node);
}

static struct proc_ctx *rr_dequeue(struct ll_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = (struct proc_ctx *)sys_slist_get(&conn->llcp.remote.pend_proc_list);
	return ctx;
}

struct proc_ctx *llcp_rr_peek(struct ll_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = (struct proc_ctx *)sys_slist_peek_head(&conn->llcp.remote.pend_proc_list);
	return ctx;
}

void llcp_rr_pause(struct ll_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = (struct proc_ctx *)sys_slist_peek_head(&conn->llcp.remote.pend_proc_list);
	if (ctx) {
		ctx->pause = 1;
	}
}

void llcp_rr_resume(struct ll_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = (struct proc_ctx *)sys_slist_peek_head(&conn->llcp.remote.pend_proc_list);
	if (ctx) {
		ctx->pause = 0;
	}
}


void llcp_rr_rx(struct ll_conn *conn, struct proc_ctx *ctx, struct node_rx_pdu *rx)
{
	switch (ctx->proc) {
	case PROC_UNKNOWN:
		/* Do nothing */
		break;
#if defined(CONFIG_BT_CTLR_LE_PING)
	case PROC_LE_PING:
		llcp_rp_comm_rx(conn, ctx, rx);
		break;
#endif /* CONFIG_BT_CTLR_LE_PING */
	case PROC_FEATURE_EXCHANGE:
		llcp_rp_comm_rx(conn, ctx, rx);
		break;
#if defined(CONFIG_BT_CTLR_MIN_USED_CHAN)
	case PROC_MIN_USED_CHANS:
		llcp_rp_comm_rx(conn, ctx, rx);
		break;
#endif /* CONFIG_BT_CTLR_MIN_USED_CHAN */
	case PROC_VERSION_EXCHANGE:
		llcp_rp_comm_rx(conn, ctx, rx);
		break;
#if defined(CONFIG_BT_CTLR_LE_ENC) && defined(CONFIG_BT_PERIPHERAL)
	case PROC_ENCRYPTION_START:
	case PROC_ENCRYPTION_PAUSE:
		llcp_rp_enc_rx(conn, ctx, rx);
		break;
#endif /* CONFIG_BT_CTLR_LE_ENC && CONFIG_BT_PERIPHERAL */
#ifdef CONFIG_BT_CTLR_PHY
	case PROC_PHY_UPDATE:
		llcp_rp_pu_rx(conn, ctx, rx);
		break;
#endif /* CONFIG_BT_CTLR_PHY */
	case PROC_CONN_UPDATE:
	case PROC_CONN_PARAM_REQ:
		llcp_rp_cu_rx(conn, ctx, rx);
		break;
	case PROC_TERMINATE:
		llcp_rp_comm_rx(conn, ctx, rx);
		break;
#if defined(CONFIG_BT_PERIPHERAL)
	case PROC_CHAN_MAP_UPDATE:
		llcp_rp_chmu_rx(conn, ctx, rx);
		break;
#endif /* CONFIG_BT_PERIPHERAL */
#if defined(CONFIG_BT_CTLR_DATA_LENGTH)
	case PROC_DATA_LENGTH_UPDATE:
		llcp_rp_comm_rx(conn, ctx, rx);
		break;
#endif /* CONFIG_BT_CTLR_DATA_LENGTH */
#if defined(CONFIG_BT_CTLR_DF_CONN_CTE_RSP)
	case PROC_CTE_REQ:
		llcp_rp_comm_rx(conn, ctx, rx);
		break;
#endif /* CONFIG_BT_CTLR_DF_CONN_CTE_RSP */
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
		break;
	}
	rr_check_done(conn, ctx);
}

void llcp_rr_tx_ack(struct ll_conn *conn, struct proc_ctx *ctx, struct node_tx *tx)
{
	switch (ctx->proc) {
#if defined(CONFIG_BT_CTLR_DATA_LENGTH)
	case PROC_DATA_LENGTH_UPDATE:
		llcp_rp_comm_tx_ack(conn, ctx, tx);
		break;
#endif /* CONFIG_BT_CTLR_DATA_LENGTH */
#ifdef CONFIG_BT_CTLR_PHY
	case PROC_PHY_UPDATE:
		llcp_rp_pu_tx_ack(conn, ctx, tx);
		break;
#endif /* CONFIG_BT_CTLR_PHY */
#if defined(CONFIG_BT_CTLR_DF_CONN_CTE_RSP)
	case PROC_CTE_REQ:
		llcp_rp_comm_tx_ack(conn, ctx, tx);
		break;
#endif /* CONFIG_BT_CTLR_DF_CONN_CTE_RSP */
	default:
		/* Ignore tx_ack */
		break;
	}

	rr_check_done(conn, ctx);
}

static void rr_act_run(struct ll_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = llcp_rr_peek(conn);

	switch (ctx->proc) {
#if defined(CONFIG_BT_CTLR_LE_PING)
	case PROC_LE_PING:
		llcp_rp_comm_run(conn, ctx, NULL);
		break;
#endif /* CONFIG_BT_CTLR_LE_PING */
	case PROC_FEATURE_EXCHANGE:
		llcp_rp_comm_run(conn, ctx, NULL);
		break;
#if defined(CONFIG_BT_CTLR_MIN_USED_CHAN)
	case PROC_MIN_USED_CHANS:
		llcp_rp_comm_run(conn, ctx, NULL);
		break;
#endif /* CONFIG_BT_CTLR_MIN_USED_CHAN */
	case PROC_VERSION_EXCHANGE:
		llcp_rp_comm_run(conn, ctx, NULL);
		break;
#if defined(CONFIG_BT_CTLR_LE_ENC) && defined(CONFIG_BT_PERIPHERAL)
	case PROC_ENCRYPTION_START:
	case PROC_ENCRYPTION_PAUSE:
		llcp_rp_enc_run(conn, ctx, NULL);
		break;
#endif /* CONFIG_BT_CTLR_LE_ENC && CONFIG_BT_PERIPHERAL */
#ifdef CONFIG_BT_CTLR_PHY
	case PROC_PHY_UPDATE:
		llcp_rp_pu_run(conn, ctx, NULL);
		break;
#endif /* CONFIG_BT_CTLR_PHY */
	case PROC_CONN_UPDATE:
	case PROC_CONN_PARAM_REQ:
		llcp_rp_cu_run(conn, ctx, NULL);
		break;
	case PROC_TERMINATE:
		llcp_rp_comm_run(conn, ctx, NULL);
		break;
#if defined(CONFIG_BT_PERIPHERAL)
	case PROC_CHAN_MAP_UPDATE:
		llcp_rp_chmu_run(conn, ctx, NULL);
		break;
#endif /* CONFIG_BT_PERIPHERAL */
#if defined(CONFIG_BT_CTLR_DATA_LENGTH)
	case PROC_DATA_LENGTH_UPDATE:
		llcp_rp_comm_run(conn, ctx, NULL);
		break;
#endif /* CONFIG_BT_CTLR_DATA_LENGTH */
#if defined(CONFIG_BT_CTLR_DF_CONN_CTE_RSP)
	case PROC_CTE_REQ:
		llcp_rp_comm_run(conn, ctx, NULL);
		break;
#endif /* CONFIG_BT_CTLR_DF_CONN_CTE_RSP */
	default:
		/* Unknown procedure */
		LL_ASSERT(0);
		break;
	}

	rr_check_done(conn, ctx);
}

static void rr_tx(struct ll_conn *conn, struct proc_ctx *ctx, uint8_t opcode)
{
	struct node_tx *tx;
	struct pdu_data *pdu;

	/* Allocate tx node */
	tx = llcp_tx_alloc(conn, ctx);
	LL_ASSERT(tx);

	pdu = (struct pdu_data *)tx->pdu;

	/* Encode LL Control PDU */
	switch (opcode) {
	case PDU_DATA_LLCTRL_TYPE_REJECT_IND:
		/* TODO(thoh): Select between LL_REJECT_IND and LL_REJECT_EXT_IND */
		llcp_pdu_encode_reject_ext_ind(pdu, conn->llcp.remote.reject_opcode,
					       BT_HCI_ERR_LL_PROC_COLLISION);
		break;
	case PDU_DATA_LLCTRL_TYPE_UNKNOWN_RSP:
		llcp_pdu_encode_unknown_rsp(ctx, pdu);
		break;
	default:
		LL_ASSERT(0);
	}

	ctx->tx_opcode = pdu->llctrl.opcode;

	/* Enqueue LL Control PDU towards LLL */
	llcp_tx_enqueue(conn, tx);
}

static void rr_act_reject(struct ll_conn *conn)
{
	struct proc_ctx *ctx = llcp_rr_peek(conn);

	LL_ASSERT(ctx != NULL);

	if (ctx->pause || !llcp_tx_alloc_peek(conn, ctx)) {
		rr_set_state(conn, RR_STATE_REJECT);
	} else {
		rr_tx(conn, ctx, PDU_DATA_LLCTRL_TYPE_REJECT_IND);

		ctx->done = 1U;
		rr_set_state(conn, RR_STATE_IDLE);
	}
}

static void rr_act_unsupported(struct ll_conn *conn)
{
	struct proc_ctx *ctx = llcp_rr_peek(conn);

	LL_ASSERT(ctx != NULL);

	if (ctx->pause || !llcp_tx_alloc_peek(conn, ctx)) {
		rr_set_state(conn, RR_STATE_UNSUPPORTED);
	} else {
		rr_tx(conn, ctx, PDU_DATA_LLCTRL_TYPE_UNKNOWN_RSP);

		ctx->done = 1U;
		rr_set_state(conn, RR_STATE_IDLE);
	}
}

static void rr_act_complete(struct ll_conn *conn)
{
	struct proc_ctx *ctx;

	rr_set_collision(conn, 0U);

	/* Dequeue pending request that just completed */
	ctx = llcp_rr_peek(conn);
	LL_ASSERT(ctx != NULL);

	ctx->done = 1U;
}

static void rr_act_connect(struct ll_conn *conn)
{
	/* TODO */
}

static void rr_act_disconnect(struct ll_conn *conn)
{
	struct proc_ctx *ctx;

	ctx = rr_dequeue(conn);

	/*
	 * we may have been disconnected in the
	 * middle of a control procedure, in  which
	 * case we need to release all contexts
	 */
	while (ctx != NULL) {
		llcp_proc_ctx_release(ctx);
		ctx = rr_dequeue(conn);
	}
}

static void rr_st_disconnect(struct ll_conn *conn, uint8_t evt, void *param)
{
	switch (evt) {
	case RR_EVT_CONNECT:
		rr_act_connect(conn);
		rr_set_state(conn, RR_STATE_IDLE);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rr_st_idle(struct ll_conn *conn, uint8_t evt, void *param)
{
	struct proc_ctx *ctx;

	switch (evt) {
	case RR_EVT_PREPARE:
		ctx = llcp_rr_peek(conn);
		if (ctx) {
			const enum proc_incompat incompat = rr_get_incompat(conn);
			const bool periph = !!(conn->lll.role == BT_HCI_ROLE_PERIPHERAL);
			const bool central = !!(conn->lll.role == BT_HCI_ROLE_CENTRAL);
			const bool with_instant = proc_with_instant(ctx);

			if (ctx->proc == PROC_TERMINATE) {
				/* Peer terminate overrides all */

				/* Run remote procedure */
				rr_act_run(conn);
				rr_set_state(conn, RR_STATE_TERMINATE);
			} else if (ctx->proc == PROC_UNKNOWN) {
				/* Unsupported procedure */

				/* Send LL_UNKNOWN_RSP */
				struct node_rx_pdu *rx = (struct node_rx_pdu *)param;
				struct pdu_data *pdu = (struct pdu_data *)rx->pdu;

				ctx->unknown_response.type = pdu->llctrl.opcode;
				rr_act_unsupported(conn);
			} else if (!with_instant || incompat == INCOMPAT_NO_COLLISION) {
				/* No collision
				 * => Run procedure
				 *
				 * Local incompatible procedure request is kept pending.
				 */

				/* Pause local incompatible procedure */
				rr_set_collision(conn, with_instant);

				/* Run remote procedure */
				rr_act_run(conn);
				rr_set_state(conn, RR_STATE_ACTIVE);
			} else if (periph && incompat == INCOMPAT_RESOLVABLE) {
				/* Peripheral collision
				 * => Run procedure
				 *
				 * Local periph procedure completes with error.
				 */

				/* Run remote procedure */
				rr_act_run(conn);
				rr_set_state(conn, RR_STATE_ACTIVE);
			} else if (with_instant && central && incompat == INCOMPAT_RESOLVABLE) {
				/* Central collision
				 * => Send reject
				 *
				 * Local central incompatible procedure continues unaffected.
				 */

				/* Send reject */
				struct node_rx_pdu *rx = (struct node_rx_pdu *)param;
				struct pdu_data *pdu = (struct pdu_data *)rx->pdu;

				conn->llcp.remote.reject_opcode = pdu->llctrl.opcode;
				rr_act_reject(conn);
			} else if (!with_instant && central && incompat == INCOMPAT_RESOLVABLE) {
				/* No collision with procedure without instant
				 * => Run procedure
				 */
				rr_act_run(conn);
				rr_set_state(conn, RR_STATE_ACTIVE);
			} else if (with_instant && incompat == INCOMPAT_RESERVED) {
				/* Protocol violation.
				 * => Disconnect
				 *
				 */

				/* TODO */
				LL_ASSERT(0);
			}
		}
		break;
	case RR_EVT_DISCONNECT:
		rr_act_disconnect(conn);
		rr_set_state(conn, RR_STATE_DISCONNECT);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}
static void rr_st_reject(struct ll_conn *conn, uint8_t evt, void *param)
{
	/* TODO */
	LL_ASSERT(0);
}

static void rr_st_unsupported(struct ll_conn *conn, uint8_t evt, void *param)
{
	rr_act_unsupported(conn);
}

static void rr_st_active(struct ll_conn *conn, uint8_t evt, void *param)
{
	switch (evt) {
	case RR_EVT_RUN:
		if (llcp_rr_peek(conn)) {
			rr_act_run(conn);
		}
		break;
	case RR_EVT_COMPLETE:
		rr_act_complete(conn);
		rr_set_state(conn, RR_STATE_IDLE);
		break;
	case RR_EVT_DISCONNECT:
		rr_act_disconnect(conn);
		rr_set_state(conn, RR_STATE_DISCONNECT);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rr_st_terminate(struct ll_conn *conn, uint8_t evt, void *param)
{
	switch (evt) {
	case RR_EVT_RUN:
		if (llcp_rr_peek(conn)) {
			rr_act_run(conn);
		}
		break;
	case RR_EVT_COMPLETE:
		rr_act_complete(conn);
		rr_set_state(conn, RR_STATE_IDLE);
		break;
	case RR_EVT_DISCONNECT:
		rr_act_disconnect(conn);
		rr_set_state(conn, RR_STATE_DISCONNECT);
		break;
	default:
		/* Ignore other evts */
		break;
	}
}

static void rr_execute_fsm(struct ll_conn *conn, uint8_t evt, void *param)
{
	switch (conn->llcp.remote.state) {
	case RR_STATE_DISCONNECT:
		rr_st_disconnect(conn, evt, param);
		break;
	case RR_STATE_IDLE:
		rr_st_idle(conn, evt, param);
		break;
	case RR_STATE_REJECT:
		rr_st_reject(conn, evt, param);
		break;
	case RR_STATE_UNSUPPORTED:
		rr_st_unsupported(conn, evt, param);
		break;
	case RR_STATE_ACTIVE:
		rr_st_active(conn, evt, param);
		break;
	case RR_STATE_TERMINATE:
		rr_st_terminate(conn, evt, param);
		break;
	default:
		/* Unknown state */
		LL_ASSERT(0);
	}
}

void llcp_rr_init(struct ll_conn *conn)
{
	rr_set_state(conn, RR_STATE_DISCONNECT);
}

void llcp_rr_prepare(struct ll_conn *conn, struct node_rx_pdu *rx)
{
	rr_execute_fsm(conn, RR_EVT_PREPARE, rx);
}

void llcp_rr_run(struct ll_conn *conn)
{
	rr_execute_fsm(conn, RR_EVT_RUN, NULL);
}

void llcp_rr_complete(struct ll_conn *conn)
{
	rr_execute_fsm(conn, RR_EVT_COMPLETE, NULL);
}

void llcp_rr_connect(struct ll_conn *conn)
{
	rr_execute_fsm(conn, RR_EVT_CONNECT, NULL);
}

void llcp_rr_disconnect(struct ll_conn *conn)
{
	rr_execute_fsm(conn, RR_EVT_DISCONNECT, NULL);
}

/*
 * Create procedure lookup table for any given PDU opcode, these are the opcodes
 * that can initiate a new remote procedure.
 *
 * NOTE: All array elements that are not initialized explicitly are
 * zero-initialized, which matches PROC_UNKNOWN.
 *
 * NOTE: When initializing an array of unknown size, the largest subscript for
 * which an initializer is specified determines the size of the array being
 * declared.
 */
BUILD_ASSERT(0 == PROC_UNKNOWN, "PROC_UNKNOWN must have the value ZERO");

enum accept_role {
	/* No role accepts this opcode */
	ACCEPT_ROLE_NONE = 0,
	/* Central role accepts this opcode */
	ACCEPT_ROLE_CENTRAL = (1 << BT_HCI_ROLE_CENTRAL),
	/* Peripheral role accepts this opcode */
	ACCEPT_ROLE_PERIPHERAL = (1 << BT_HCI_ROLE_PERIPHERAL),
	/* Both roles accepts this opcode */
	ACCEPT_ROLE_BOTH = (ACCEPT_ROLE_CENTRAL | ACCEPT_ROLE_PERIPHERAL),
};

struct proc_role {
	enum llcp_proc proc;
	enum accept_role accept;
};

static const struct proc_role new_proc_lut[] = {
#if defined(CONFIG_BT_PERIPHERAL)
	[PDU_DATA_LLCTRL_TYPE_CONN_UPDATE_IND] = { PROC_CONN_UPDATE, ACCEPT_ROLE_PERIPHERAL },
	[PDU_DATA_LLCTRL_TYPE_CHAN_MAP_IND] = { PROC_CHAN_MAP_UPDATE, ACCEPT_ROLE_PERIPHERAL },
#endif /* CONFIG_BT_PERIPHERAL */
	[PDU_DATA_LLCTRL_TYPE_TERMINATE_IND] = { PROC_TERMINATE, ACCEPT_ROLE_BOTH },
#if defined(CONFIG_BT_CTLR_LE_ENC) && defined(CONFIG_BT_PERIPHERAL)
	[PDU_DATA_LLCTRL_TYPE_ENC_REQ] = { PROC_ENCRYPTION_START, ACCEPT_ROLE_PERIPHERAL },
#endif /* CONFIG_BT_CTLR_LE_ENC && CONFIG_BT_PERIPHERAL */
	[PDU_DATA_LLCTRL_TYPE_ENC_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
	[PDU_DATA_LLCTRL_TYPE_START_ENC_REQ] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
	[PDU_DATA_LLCTRL_TYPE_START_ENC_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
	[PDU_DATA_LLCTRL_TYPE_UNKNOWN_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
	[PDU_DATA_LLCTRL_TYPE_FEATURE_REQ] = { PROC_FEATURE_EXCHANGE, ACCEPT_ROLE_PERIPHERAL },
	[PDU_DATA_LLCTRL_TYPE_FEATURE_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
#if defined(CONFIG_BT_CTLR_LE_ENC) && defined(CONFIG_BT_PERIPHERAL)
	[PDU_DATA_LLCTRL_TYPE_PAUSE_ENC_REQ] = { PROC_ENCRYPTION_PAUSE, ACCEPT_ROLE_PERIPHERAL },
#endif /* CONFIG_BT_CTLR_LE_ENC && CONFIG_BT_PERIPHERAL */
	[PDU_DATA_LLCTRL_TYPE_PAUSE_ENC_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
	[PDU_DATA_LLCTRL_TYPE_VERSION_IND] = { PROC_VERSION_EXCHANGE, ACCEPT_ROLE_BOTH },
	[PDU_DATA_LLCTRL_TYPE_REJECT_IND] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
#if defined(CONFIG_BT_CTLR_PER_INIT_FEAT_XCHG) && defined(CONFIG_BT_CENTRAL)
	[PDU_DATA_LLCTRL_TYPE_PER_INIT_FEAT_XCHG] = { PROC_FEATURE_EXCHANGE, ACCEPT_ROLE_CENTRAL },
#endif /* CONFIG_BT_CTLR_PER_INIT_FEAT_XCHG && CONFIG_BT_CENTRAL */
#if defined(CONFIG_BT_CTLR_CONN_PARAM_REQ)
	[PDU_DATA_LLCTRL_TYPE_CONN_PARAM_REQ] = { PROC_CONN_PARAM_REQ, ACCEPT_ROLE_BOTH },
#endif /* CONFIG_BT_CTLR_CONN_PARAM_REQ */
	[PDU_DATA_LLCTRL_TYPE_CONN_PARAM_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
	[PDU_DATA_LLCTRL_TYPE_REJECT_EXT_IND] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
#if defined(CONFIG_BT_CTLR_LE_PING)
	[PDU_DATA_LLCTRL_TYPE_PING_REQ]	= { PROC_LE_PING, ACCEPT_ROLE_BOTH },
#endif /* CONFIG_BT_CTLR_LE_PING */
	[PDU_DATA_LLCTRL_TYPE_PING_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
#if defined(CONFIG_BT_CTLR_DATA_LENGTH)
	[PDU_DATA_LLCTRL_TYPE_LENGTH_REQ] = { PROC_DATA_LENGTH_UPDATE, ACCEPT_ROLE_BOTH },
#endif /* CONFIG_BT_CTLR_DATA_LENGTH */
	[PDU_DATA_LLCTRL_TYPE_LENGTH_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
#if defined(CONFIG_BT_CTLR_PHY)
	[PDU_DATA_LLCTRL_TYPE_PHY_REQ] = { PROC_PHY_UPDATE, ACCEPT_ROLE_BOTH },
#endif /* CONFIG_BT_CTLR_PHY */
	[PDU_DATA_LLCTRL_TYPE_PHY_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
	[PDU_DATA_LLCTRL_TYPE_PHY_UPD_IND] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
#if defined(CONFIG_BT_CTLR_MIN_USED_CHAN) && defined(CONFIG_BT_CENTRAL)
	[PDU_DATA_LLCTRL_TYPE_MIN_USED_CHAN_IND] = { PROC_MIN_USED_CHANS, ACCEPT_ROLE_CENTRAL },
#endif /* CONFIG_BT_CTLR_MIN_USED_CHAN && CONFIG_BT_CENTRAL */
#if defined(CONFIG_BT_CTLR_DF_CONN_CTE_REQ)
	[PDU_DATA_LLCTRL_TYPE_CTE_REQ] = { PROC_CTE_REQ, ACCEPT_ROLE_BOTH },
#endif /* CONFIG_BT_CTLR_DF_CONN_CTE_REQ */
	[PDU_DATA_LLCTRL_TYPE_CTE_RSP] = { PROC_UNKNOWN, ACCEPT_ROLE_NONE },
};

void llcp_rr_new(struct ll_conn *conn, struct node_rx_pdu *rx)
{
	struct proc_ctx *ctx;
	struct pdu_data *pdu;
	uint8_t proc = PROC_UNKNOWN;

	pdu = (struct pdu_data *)rx->pdu;


	/* Is this a valid opcode */
	if (pdu->llctrl.opcode < ARRAY_SIZE(new_proc_lut)) {
		/* Lookup procedure */
		uint8_t role_mask  = (1 << conn->lll.role);
		struct proc_role pr = new_proc_lut[pdu->llctrl.opcode];

		if (pr.accept & role_mask) {
			proc = pr.proc;
		}
	}

	if (proc == PROC_TERMINATE) {
		rr_abort(conn);
	}

	ctx = llcp_create_remote_procedure(proc);
	if (!ctx) {
		return;
	}

	/* Enqueue procedure */
	rr_enqueue(conn, ctx);

	/* Prepare procedure */
	llcp_rr_prepare(conn, rx);

	rr_check_done(conn, ctx);

	/* Handle PDU */
	ctx = llcp_rr_peek(conn);
	if (ctx) {
		llcp_rr_rx(conn, ctx, rx);
	}
}

static void rr_abort(struct ll_conn *conn)
{
	struct proc_ctx *ctx;

	/* Flush all pending procedures */
	ctx = rr_dequeue(conn);
	while (ctx) {
		llcp_proc_ctx_release(ctx);
		ctx = rr_dequeue(conn);
	}

	/* TODO(thoh): Whats missing here ??? */
	rr_set_collision(conn, 0U);
	rr_set_state(conn, RR_STATE_IDLE);
}

#ifdef ZTEST_UNITTEST

bool rr_is_disconnected(struct ll_conn *conn)
{
	return conn->llcp.remote.state == RR_STATE_DISCONNECT;
}

bool rr_is_idle(struct ll_conn *conn)
{
	return conn->llcp.remote.state == RR_STATE_IDLE;
}

void test_int_remote_pending_requests(void)
{
	struct ll_conn conn;
	struct proc_ctx *peek_ctx;
	struct proc_ctx *dequeue_ctx;
	struct proc_ctx ctx;

	ull_cp_init();
	ull_tx_q_init(&conn.tx_q);
	ull_llcp_init(&conn);

	peek_ctx = llcp_rr_peek(&conn);
	zassert_is_null(peek_ctx, NULL);

	dequeue_ctx = rr_dequeue(&conn);
	zassert_is_null(dequeue_ctx, NULL);

	rr_enqueue(&conn, &ctx);
	peek_ctx = (struct proc_ctx *)sys_slist_peek_head(&conn.llcp.remote.pend_proc_list);
	zassert_equal_ptr(peek_ctx, &ctx, NULL);

	peek_ctx = llcp_rr_peek(&conn);
	zassert_equal_ptr(peek_ctx, &ctx, NULL);

	dequeue_ctx = rr_dequeue(&conn);
	zassert_equal_ptr(dequeue_ctx, &ctx, NULL);

	peek_ctx = llcp_rr_peek(&conn);
	zassert_is_null(peek_ctx, NULL);

	dequeue_ctx = rr_dequeue(&conn);
	zassert_is_null(dequeue_ctx, NULL);
}

#endif
