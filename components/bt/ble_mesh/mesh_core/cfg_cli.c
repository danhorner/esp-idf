/*  Bluetooth Mesh */

/*
 * Copyright (c) 2017 Intel Corporation
 * Additional Copyright (c) 2018 Espressif Systems (Shanghai) PTE LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include "mesh_types.h"
#include "mesh_util.h"
#include "mesh_kernel.h"

#include "mesh.h"
#include "sdkconfig.h"
#include "osi/allocator.h"

#if CONFIG_BT_MESH

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_MODEL)
#include "mesh_trace.h"
#include "cfg_cli.h"
#include "foundation.h"
#include "common.h"
#include "btc_ble_mesh_config_client.h"

#define CID_NVAL 0xffff

s32_t config_msg_timeout;

static bt_mesh_config_client_t *cli;

static const bt_mesh_client_op_pair_t cfg_op_pair[] = {
    { OP_BEACON_GET,           OP_BEACON_STATUS        },
    { OP_BEACON_SET,           OP_BEACON_STATUS        },
    { OP_DEV_COMP_DATA_GET,    OP_DEV_COMP_DATA_STATUS },
    { OP_DEFAULT_TTL_GET,      OP_DEFAULT_TTL_STATUS   },
    { OP_DEFAULT_TTL_SET,      OP_DEFAULT_TTL_STATUS   },
    { OP_GATT_PROXY_GET,       OP_GATT_PROXY_STATUS    },
    { OP_GATT_PROXY_SET,       OP_GATT_PROXY_STATUS    },
    { OP_RELAY_GET,            OP_RELAY_STATUS         },
    { OP_RELAY_SET,            OP_RELAY_STATUS         },
    { OP_MOD_PUB_GET,          OP_MOD_PUB_STATUS       },
    { OP_MOD_PUB_SET,          OP_MOD_PUB_STATUS       },
    { OP_MOD_PUB_VA_SET,       OP_MOD_PUB_STATUS       },
    { OP_MOD_SUB_ADD,          OP_MOD_SUB_STATUS       },
    { OP_MOD_SUB_VA_ADD,       OP_MOD_SUB_STATUS       },
    { OP_MOD_SUB_DEL,          OP_MOD_SUB_STATUS       },
    { OP_MOD_SUB_VA_DEL,       OP_MOD_SUB_STATUS       },
    { OP_MOD_SUB_OVERWRITE,    OP_MOD_SUB_STATUS       },
    { OP_MOD_SUB_VA_OVERWRITE, OP_MOD_SUB_STATUS       },
    { OP_MOD_SUB_DEL_ALL,      OP_MOD_SUB_STATUS       },
    { OP_MOD_SUB_GET,          OP_MOD_SUB_LIST         },
    { OP_MOD_SUB_GET_VND,      OP_MOD_SUB_LIST_VND     },
    { OP_NET_KEY_ADD,          OP_NET_KEY_STATUS       },
    { OP_NET_KEY_UPDATE,       OP_NET_KEY_STATUS       },
    { OP_NET_KEY_DEL,          OP_NET_KEY_STATUS       },
    { OP_NET_KEY_GET,          OP_NET_KEY_LIST         },
    { OP_APP_KEY_ADD,          OP_APP_KEY_STATUS       },
    { OP_APP_KEY_UPDATE,       OP_APP_KEY_STATUS       },
    { OP_APP_KEY_DEL,          OP_APP_KEY_STATUS       },
    { OP_APP_KEY_GET,          OP_APP_KEY_LIST         },
    { OP_NODE_IDENTITY_GET,    OP_NODE_IDENTITY_STATUS },
    { OP_NODE_IDENTITY_SET,    OP_NODE_IDENTITY_STATUS },
    { OP_MOD_APP_BIND,         OP_MOD_APP_STATUS       },
    { OP_MOD_APP_UNBIND,       OP_MOD_APP_STATUS       },
    { OP_SIG_MOD_APP_GET,      OP_SIG_MOD_APP_LIST     },
    { OP_VND_MOD_APP_GET,      OP_VND_MOD_APP_LIST     },
    { OP_NODE_RESET,           OP_NODE_RESET_STATUS    },
    { OP_FRIEND_GET,           OP_FRIEND_STATUS        },
    { OP_FRIEND_SET,           OP_FRIEND_STATUS        },
    { OP_KRP_GET,              OP_KRP_STATUS           },
    { OP_KRP_SET,              OP_KRP_STATUS           },
    { OP_HEARTBEAT_PUB_GET,    OP_HEARTBEAT_PUB_STATUS },
    { OP_HEARTBEAT_PUB_SET,    OP_HEARTBEAT_PUB_STATUS },
    { OP_HEARTBEAT_SUB_GET,    OP_HEARTBEAT_SUB_STATUS },
    { OP_HEARTBEAT_SUB_SET,    OP_HEARTBEAT_SUB_STATUS },
    { OP_LPN_TIMEOUT_GET,      OP_LPN_TIMEOUT_STATUS   },
    { OP_NET_TRANSMIT_GET,     OP_NET_TRANSMIT_STATUS  },
    { OP_NET_TRANSMIT_SET,     OP_NET_TRANSMIT_STATUS  },
};

static void timeout_handler(struct k_work *work)
{
    bt_mesh_config_client_t *client   = NULL;
    config_internal_data_t  *internal = NULL;
    bt_mesh_client_node_t   *node     = NULL;

    BT_WARN("Receive configuration status message timeout");

    node = CONTAINER_OF(work, bt_mesh_client_node_t, timer.work);
    if (!node || !node->ctx.model) {
        BT_ERR("%s: node parameter is NULL", __func__);
        return;
    }

    client = (bt_mesh_config_client_t *)node->ctx.model->user_data;
    if (!client) {
        BT_ERR("%s: model user_data is NULL", __func__);
        return;
    }

    internal = (config_internal_data_t *)client->internal_data;
    if (!internal) {
        BT_ERR("%s: internal_data is NULL", __func__);
        return;
    }

    bt_mesh_callback_config_status_to_btc(node->opcode, 0x03, node->ctx.model,
                                          &node->ctx, NULL, 0);

    sys_slist_find_and_remove(&internal->queue, &node->client_node);
    osi_free(node);

    return;
}

static void cfg_client_cancel(struct bt_mesh_model *model,
                              struct bt_mesh_msg_ctx *ctx,
                              void *status, size_t len)
{
    config_internal_data_t *data = NULL;
    bt_mesh_client_node_t  *node = NULL;
    struct net_buf_simple buf = {0};
    u8_t evt_type = 0xFF;

    if (!model || !ctx) {
        BT_ERR("%s: invalid parameter", __func__);
        return;
    }

    data = (config_internal_data_t *)cli->internal_data;
    if (!data) {
        BT_ERR("%s: config client internal_data is NULL", __func__);
        return;
    }

    /* If it is a publish message, sent to the user directly. */
    buf.data = (u8_t *)status;
    buf.len  = (u16_t)len;
    node = bt_mesh_is_model_message_publish(model, ctx, &buf, true);
    if (!node) {
        BT_DBG("Unexpected config status message 0x%x", ctx->recv_op);
    } else {
        switch (node->opcode) {
        case OP_BEACON_GET:
        case OP_DEV_COMP_DATA_GET:
        case OP_DEFAULT_TTL_GET:
        case OP_GATT_PROXY_GET:
        case OP_RELAY_GET:
        case OP_MOD_PUB_GET:
        case OP_MOD_SUB_GET:
        case OP_MOD_SUB_GET_VND:
        case OP_NET_KEY_GET:
        case OP_APP_KEY_GET:
        case OP_NODE_IDENTITY_GET:
        case OP_SIG_MOD_APP_GET:
        case OP_VND_MOD_APP_GET:
        case OP_FRIEND_GET:
        case OP_KRP_GET:
        case OP_HEARTBEAT_PUB_GET:
        case OP_HEARTBEAT_SUB_GET:
        case OP_LPN_TIMEOUT_GET:
        case OP_NET_TRANSMIT_GET:
            evt_type = 0x00;
            break;
        case OP_BEACON_SET:
        case OP_DEFAULT_TTL_SET:
        case OP_GATT_PROXY_SET:
        case OP_RELAY_SET:
        case OP_MOD_PUB_SET:
        case OP_MOD_PUB_VA_SET:
        case OP_MOD_SUB_ADD:
        case OP_MOD_SUB_VA_ADD:
        case OP_MOD_SUB_DEL:
        case OP_MOD_SUB_VA_DEL:
        case OP_MOD_SUB_OVERWRITE:
        case OP_MOD_SUB_VA_OVERWRITE:
        case OP_MOD_SUB_DEL_ALL:
        case OP_NET_KEY_ADD:
        case OP_NET_KEY_UPDATE:
        case OP_NET_KEY_DEL:
        case OP_APP_KEY_ADD:
        case OP_APP_KEY_UPDATE:
        case OP_APP_KEY_DEL:
        case OP_NODE_IDENTITY_SET:
        case OP_MOD_APP_BIND:
        case OP_MOD_APP_UNBIND:
        case OP_NODE_RESET:
        case OP_FRIEND_SET:
        case OP_KRP_SET:
        case OP_HEARTBEAT_PUB_SET:
        case OP_HEARTBEAT_SUB_SET:
        case OP_NET_TRANSMIT_SET:
            evt_type = 0x01;
            break;
        default:
            break;
        }

        bt_mesh_callback_config_status_to_btc(node->opcode, evt_type, model,
                                              ctx, (const u8_t *)status, len);
        // Don't forget to release the node at the end.
        bt_mesh_client_free_node(&data->queue, node);
    }

    switch (ctx->recv_op) {
    case OP_DEV_COMP_DATA_STATUS: {
        struct bt_mesh_cfg_comp_data_status *val;
        val = (struct bt_mesh_cfg_comp_data_status *)status;
        bt_mesh_free_buf(val->comp_data);
        break;
    }
    default:
        break;
    }
}

static void comp_data_status(struct bt_mesh_model *model,
                             struct bt_mesh_msg_ctx *ctx,
                             struct net_buf_simple *buf)
{
    struct bt_mesh_cfg_comp_data_status status = {0};

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status.page = net_buf_simple_pull_u8(buf);
    status.comp_data = bt_mesh_alloc_buf(buf->len);
    if (!status.comp_data) {
        BT_ERR("%s: allocate memory for comp_data fail", __func__);
        return;
    }

    net_buf_simple_init(status.comp_data, 0);
    net_buf_simple_add_mem(status.comp_data, buf->data, buf->len);

    cfg_client_cancel(model, ctx, &status, sizeof(struct bt_mesh_cfg_comp_data_status));
}

static void state_status_u8(struct bt_mesh_model *model,
                            struct bt_mesh_msg_ctx *ctx,
                            struct net_buf_simple *buf)
{
    u8_t status = 0;

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status = net_buf_simple_pull_u8(buf);

    cfg_client_cancel(model, ctx, &status, sizeof(u8_t));
}

static void beacon_status(struct bt_mesh_model *model,
                          struct bt_mesh_msg_ctx *ctx,
                          struct net_buf_simple *buf)
{
    state_status_u8(model, ctx, buf);
}

static void ttl_status(struct bt_mesh_model *model,
                       struct bt_mesh_msg_ctx *ctx,
                       struct net_buf_simple *buf)
{
    state_status_u8(model, ctx, buf);
}

static void friend_status(struct bt_mesh_model *model,
                          struct bt_mesh_msg_ctx *ctx,
                          struct net_buf_simple *buf)
{
    state_status_u8(model, ctx, buf);
}

static void gatt_proxy_status(struct bt_mesh_model *model,
                              struct bt_mesh_msg_ctx *ctx,
                              struct net_buf_simple *buf)
{
    state_status_u8(model, ctx, buf);
}

static void relay_status(struct bt_mesh_model *model,
                         struct bt_mesh_msg_ctx *ctx,
                         struct net_buf_simple *buf)
{
    struct bt_mesh_cfg_relay_status status = {0};

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status.relay      = net_buf_simple_pull_u8(buf);
    status.retransmit = net_buf_simple_pull_u8(buf);

    cfg_client_cancel(model, ctx, &status, sizeof(struct bt_mesh_cfg_relay_status));
}

static void net_key_status(struct bt_mesh_model *model,
                           struct bt_mesh_msg_ctx *ctx,
                           struct net_buf_simple *buf)
{
    struct bt_mesh_cfg_netkey_status status = {0};
    u16_t app_idx;

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status.status = net_buf_simple_pull_u8(buf);
    key_idx_unpack(buf, &status.net_idx, &app_idx);

    cfg_client_cancel(model, ctx, &status, sizeof(struct bt_mesh_cfg_netkey_status));
}

static void app_key_status(struct bt_mesh_model *model,
                           struct bt_mesh_msg_ctx *ctx,
                           struct net_buf_simple *buf)
{
    struct bt_mesh_cfg_appkey_status status = {0};

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status.status = net_buf_simple_pull_u8(buf);
    key_idx_unpack(buf, &status.net_idx, &status.app_idx);

    cfg_client_cancel(model, ctx, &status, sizeof(struct bt_mesh_cfg_appkey_status));
}

static void mod_app_status(struct bt_mesh_model *model,
                           struct bt_mesh_msg_ctx *ctx,
                           struct net_buf_simple *buf)
{
    struct bt_mesh_cfg_mod_app_status status = {0};

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status.status    = net_buf_simple_pull_u8(buf);
    status.elem_addr = net_buf_simple_pull_le16(buf);
    status.app_idx   = net_buf_simple_pull_le16(buf);
    if (buf->len >= 4) {
        status.cid = net_buf_simple_pull_le16(buf);
    } else {
        status.cid = CID_NVAL;
    }
    status.mod_id = net_buf_simple_pull_le16(buf);

    cfg_client_cancel(model, ctx, &status, sizeof(struct bt_mesh_cfg_mod_app_status));
}

static void mod_pub_status(struct bt_mesh_model *model,
                           struct bt_mesh_msg_ctx *ctx,
                           struct net_buf_simple *buf)
{
    struct bt_mesh_cfg_mod_pub_status status = {0};

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status.status    = net_buf_simple_pull_u8(buf);
    status.elem_addr = net_buf_simple_pull_le16(buf);
    status.addr      = net_buf_simple_pull_le16(buf);
    status.app_idx   = net_buf_simple_pull_le16(buf);
    status.cred_flag = (status.app_idx & BIT(12));
    status.app_idx  &= BIT_MASK(12);
    status.ttl       = net_buf_simple_pull_u8(buf);
    status.period    = net_buf_simple_pull_u8(buf);
    status.transmit  = net_buf_simple_pull_u8(buf);
    if (buf->len >= 4) {
        status.cid = net_buf_simple_pull_le16(buf);
    } else {
        status.cid = CID_NVAL;
    }
    status.mod_id = net_buf_simple_pull_le16(buf);

    cfg_client_cancel(model, ctx, &status, sizeof(struct bt_mesh_cfg_mod_pub_status));
}

static void mod_sub_status(struct bt_mesh_model *model,
                           struct bt_mesh_msg_ctx *ctx,
                           struct net_buf_simple *buf)
{
    struct bt_mesh_cfg_mod_sub_status status = {0};

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status.status    = net_buf_simple_pull_u8(buf);
    status.elem_addr = net_buf_simple_pull_le16(buf);
    status.sub_addr  = net_buf_simple_pull_le16(buf);
    if (buf->len >= 4) {
        status.cid = net_buf_simple_pull_le16(buf);
    } else {
        status.cid = CID_NVAL;
    }
    status.mod_id = net_buf_simple_pull_le16(buf);

    cfg_client_cancel(model, ctx, &status, sizeof(struct bt_mesh_cfg_mod_sub_status));
}

static void hb_sub_status(struct bt_mesh_model *model,
                          struct bt_mesh_msg_ctx *ctx,
                          struct net_buf_simple *buf)
{
    struct bt_mesh_cfg_hb_sub_status status = {0};

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status.status = net_buf_simple_pull_u8(buf);
    status.src    = net_buf_simple_pull_le16(buf);
    status.dst    = net_buf_simple_pull_le16(buf);
    status.period = net_buf_simple_pull_u8(buf);
    status.count  = net_buf_simple_pull_u8(buf);
    status.min    = net_buf_simple_pull_u8(buf);
    status.max    = net_buf_simple_pull_u8(buf);

    cfg_client_cancel(model, ctx, &status, sizeof(struct bt_mesh_cfg_hb_sub_status));
}

static void hb_pub_status(struct bt_mesh_model *model,
                          struct bt_mesh_msg_ctx *ctx,
                          struct net_buf_simple *buf)
{
    struct bt_mesh_cfg_hb_pub_status status = {0};

    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    status.status  = net_buf_simple_pull_u8(buf);
    status.dst     = net_buf_simple_pull_le16(buf);
    status.count   = net_buf_simple_pull_u8(buf);
    status.period  = net_buf_simple_pull_u8(buf);
    status.ttl     = net_buf_simple_pull_u8(buf);
    status.feat    = net_buf_simple_pull_u8(buf);
    status.net_idx = net_buf_simple_pull_u8(buf);

    cfg_client_cancel(model, ctx, &status, sizeof(struct bt_mesh_cfg_hb_sub_status));
}

static void node_reset_status(struct bt_mesh_model *model,
                              struct bt_mesh_msg_ctx *ctx,
                              struct net_buf_simple *buf)
{
    BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
           ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
           bt_hex(buf->data, buf->len));

    cfg_client_cancel(model, ctx, NULL, 0);
}

const struct bt_mesh_model_op bt_mesh_cfg_cli_op[] = {
    { OP_DEV_COMP_DATA_STATUS,   15,  comp_data_status  },
    { OP_BEACON_STATUS,          1,   beacon_status     },
    { OP_DEFAULT_TTL_STATUS,     1,   ttl_status        },
    { OP_FRIEND_STATUS,          1,   friend_status     },
    { OP_GATT_PROXY_STATUS,      1,   gatt_proxy_status },
    { OP_RELAY_STATUS,           2,   relay_status      },
    { OP_NET_KEY_STATUS,         3,   net_key_status    },
    { OP_APP_KEY_STATUS,         4,   app_key_status    },
    { OP_MOD_APP_STATUS,         7,   mod_app_status    },
    { OP_MOD_PUB_STATUS,         12,  mod_pub_status    },
    { OP_MOD_SUB_STATUS,         7,   mod_sub_status    },
    { OP_HEARTBEAT_SUB_STATUS,   9,   hb_sub_status     },
    { OP_HEARTBEAT_PUB_STATUS,   10,  hb_pub_status     },
    { OP_NODE_RESET_STATUS,      0,   node_reset_status },
    BT_MESH_MODEL_OP_END,
};

int bt_mesh_cfg_comp_data_get(struct bt_mesh_msg_ctx *ctx, u8_t page)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 1 + 4);
    int err;

    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_DEV_COMP_DATA_GET);
    net_buf_simple_add_u8(msg, page);

    err = bt_mesh_client_send_msg(cli->model, OP_DEV_COMP_DATA_GET, ctx,
                                  msg, timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

static int get_state_u8(struct bt_mesh_msg_ctx *ctx, u32_t op)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 0 + 4);
    int err;

    bt_mesh_model_msg_init(msg, op);

    err = bt_mesh_client_send_msg(cli->model, op, ctx, msg, timeout_handler,
                                  config_msg_timeout, true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

static int set_state_u8(struct bt_mesh_msg_ctx *ctx, u32_t op, u8_t new_val)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 1 + 4);
    int err;

    bt_mesh_model_msg_init(msg, op);
    net_buf_simple_add_u8(msg, new_val);

    err = bt_mesh_client_send_msg(cli->model, op, ctx, msg, timeout_handler,
                                  config_msg_timeout, true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_beacon_get(struct bt_mesh_msg_ctx *ctx)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return get_state_u8(ctx, OP_BEACON_GET);
}

int bt_mesh_cfg_beacon_set(struct bt_mesh_msg_ctx *ctx, u8_t val)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return set_state_u8(ctx, OP_BEACON_SET, val);
}

int bt_mesh_cfg_ttl_get(struct bt_mesh_msg_ctx *ctx)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return get_state_u8(ctx, OP_DEFAULT_TTL_GET);
}

int bt_mesh_cfg_ttl_set(struct bt_mesh_msg_ctx *ctx, u8_t val)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return set_state_u8(ctx, OP_DEFAULT_TTL_SET, val);
}

int bt_mesh_cfg_friend_get(struct bt_mesh_msg_ctx *ctx)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return get_state_u8(ctx, OP_FRIEND_GET);
}

int bt_mesh_cfg_friend_set(struct bt_mesh_msg_ctx *ctx, u8_t val)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return set_state_u8(ctx, OP_FRIEND_SET, val);
}

int bt_mesh_cfg_gatt_proxy_get(struct bt_mesh_msg_ctx *ctx)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return get_state_u8(ctx, OP_GATT_PROXY_GET);
}

int bt_mesh_cfg_gatt_proxy_set(struct bt_mesh_msg_ctx *ctx, u8_t val)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return set_state_u8(ctx, OP_GATT_PROXY_SET, val);
}

int bt_mesh_cfg_relay_get(struct bt_mesh_msg_ctx *ctx)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 0 + 4);
    int err;

    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_RELAY_GET);

    err = bt_mesh_client_send_msg(cli->model, OP_RELAY_GET, ctx, msg,
                                  timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_relay_set(struct bt_mesh_msg_ctx *ctx, u8_t new_relay,
                          u8_t new_transmit)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 2 + 4);
    int err;

    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_RELAY_SET);
    net_buf_simple_add_u8(msg, new_relay);
    net_buf_simple_add_u8(msg, new_transmit);

    err = bt_mesh_client_send_msg(cli->model, OP_RELAY_SET, ctx, msg,
                                  timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_net_key_add(struct bt_mesh_msg_ctx *ctx, u16_t key_net_idx,
                            const u8_t net_key[16])
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 18 + 4);
    int err;

    if (!ctx || !ctx->addr || !net_key) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_NET_KEY_ADD);
    net_buf_simple_add_le16(msg, key_net_idx);
    net_buf_simple_add_mem(msg, net_key, 16);

    err = bt_mesh_client_send_msg(cli->model, OP_NET_KEY_ADD, ctx, msg,
                                  timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_app_key_add(struct bt_mesh_msg_ctx *ctx, u16_t key_net_idx,
                            u16_t key_app_idx, const u8_t app_key[16])
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(1 + 19 + 4);
    int err;

    if (!ctx || !ctx->addr || !app_key) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_APP_KEY_ADD);
    key_idx_pack(msg, key_net_idx, key_app_idx);
    net_buf_simple_add_mem(msg, app_key, 16);

    err = bt_mesh_client_send_msg(cli->model, OP_APP_KEY_ADD, ctx, msg,
                                  timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

static int mod_app_bind(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                        u16_t mod_app_idx, u16_t mod_id, u16_t cid)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 8 + 4);
    int err;

    bt_mesh_model_msg_init(msg, OP_MOD_APP_BIND);
    net_buf_simple_add_le16(msg, elem_addr);
    net_buf_simple_add_le16(msg, mod_app_idx);
    if (cid != CID_NVAL) {
        net_buf_simple_add_le16(msg, cid);
    }
    net_buf_simple_add_le16(msg, mod_id);

    err = bt_mesh_client_send_msg(cli->model, OP_MOD_APP_BIND, ctx, msg,
                                  timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_mod_app_bind(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                             u16_t mod_app_idx, u16_t mod_id)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return mod_app_bind(ctx, elem_addr, mod_app_idx, mod_id, CID_NVAL);
}

int bt_mesh_cfg_mod_app_bind_vnd(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                                 u16_t mod_app_idx, u16_t mod_id, u16_t cid)
{
    if (!ctx || !ctx->addr || cid == CID_NVAL) {
        return -EINVAL;
    }
    return mod_app_bind(ctx, elem_addr, mod_app_idx, mod_id, cid);
}

static int mod_sub(u32_t op, struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                   u16_t sub_addr, u16_t mod_id, u16_t cid)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 8 + 4);
    int err;

    bt_mesh_model_msg_init(msg, op);
    net_buf_simple_add_le16(msg, elem_addr);
    net_buf_simple_add_le16(msg, sub_addr);
    if (cid != CID_NVAL) {
        net_buf_simple_add_le16(msg, cid);
    }
    net_buf_simple_add_le16(msg, mod_id);

    err = bt_mesh_client_send_msg(cli->model, op, ctx, msg, timeout_handler,
                                  config_msg_timeout, true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_mod_sub_add(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                            u16_t sub_addr, u16_t mod_id)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return mod_sub(OP_MOD_SUB_ADD, ctx, elem_addr, sub_addr, mod_id, CID_NVAL);
}

int bt_mesh_cfg_mod_sub_add_vnd(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                                u16_t sub_addr, u16_t mod_id, u16_t cid)
{
    if (!ctx || !ctx->addr || cid == CID_NVAL) {
        return -EINVAL;
    }
    return mod_sub(OP_MOD_SUB_ADD, ctx, elem_addr, sub_addr, mod_id, cid);
}

int bt_mesh_cfg_mod_sub_del(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                            u16_t sub_addr, u16_t mod_id)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return mod_sub(OP_MOD_SUB_DEL, ctx, elem_addr, sub_addr, mod_id, CID_NVAL);
}

int bt_mesh_cfg_mod_sub_del_vnd(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                                u16_t sub_addr, u16_t mod_id, u16_t cid)
{
    if (!ctx || !ctx->addr || cid == CID_NVAL) {
        return -EINVAL;
    }
    return mod_sub(OP_MOD_SUB_DEL, ctx, elem_addr, sub_addr, mod_id, cid);
}

int bt_mesh_cfg_mod_sub_overwrite(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                                  u16_t sub_addr, u16_t mod_id)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return mod_sub(OP_MOD_SUB_OVERWRITE, ctx, elem_addr, sub_addr, mod_id, CID_NVAL);
}

int bt_mesh_cfg_mod_sub_overwrite_vnd(struct bt_mesh_msg_ctx *ctx,
                                      u16_t elem_addr, u16_t sub_addr,
                                      u16_t mod_id, u16_t cid)
{
    if (!ctx || !ctx->addr || cid == CID_NVAL) {
        return -EINVAL;
    }
    return mod_sub(OP_MOD_SUB_OVERWRITE, ctx, elem_addr, sub_addr, mod_id, cid);
}

static int mod_sub_va(u32_t op, struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                      const u8_t label[16], u16_t mod_id, u16_t cid)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 22 + 4);
    int err;

    BT_DBG("net_idx 0x%04x addr 0x%04x elem_addr 0x%04x label %s",
           ctx->net_idx, ctx->addr, elem_addr, label);
    BT_DBG("mod_id 0x%04x cid 0x%04x", mod_id, cid);

    bt_mesh_model_msg_init(msg, op);
    net_buf_simple_add_le16(msg, elem_addr);
    net_buf_simple_add_mem(msg, label, 16);
    if (cid != CID_NVAL) {
        net_buf_simple_add_le16(msg, cid);
    }
    net_buf_simple_add_le16(msg, mod_id);

    err = bt_mesh_client_send_msg(cli->model, op, ctx, msg, timeout_handler,
                                  config_msg_timeout, true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_mod_sub_va_add(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                               const u8_t label[16], u16_t mod_id)
{
    if (!ctx || !ctx->addr || !label) {
        return -EINVAL;
    }
    return mod_sub_va(OP_MOD_SUB_VA_ADD, ctx, elem_addr, label, mod_id, CID_NVAL);
}

int bt_mesh_cfg_mod_sub_va_add_vnd(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                                   const u8_t label[16], u16_t mod_id, u16_t cid)
{
    if (!ctx || !ctx->addr || !label || cid == CID_NVAL) {
        return -EINVAL;
    }
    return mod_sub_va(OP_MOD_SUB_VA_ADD, ctx, elem_addr, label, mod_id, cid);
}

int bt_mesh_cfg_mod_sub_va_del(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                               const u8_t label[16], u16_t mod_id)
{
    if (!ctx || !ctx->addr || !label) {
        return -EINVAL;
    }
    return mod_sub_va(OP_MOD_SUB_VA_DEL, ctx, elem_addr, label, mod_id, CID_NVAL);
}

int bt_mesh_cfg_mod_sub_va_del_vnd(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                                   const u8_t label[16], u16_t mod_id, u16_t cid)
{
    if (!ctx || !ctx->addr || !label || cid == CID_NVAL) {
        return -EINVAL;
    }
    return mod_sub_va(OP_MOD_SUB_VA_DEL, ctx, elem_addr, label, mod_id, cid);
}

int bt_mesh_cfg_mod_sub_va_overwrite(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                                     const u8_t label[16], u16_t mod_id)
{
    if (!ctx || !ctx->addr || !label) {
        return -EINVAL;
    }
    return mod_sub_va(OP_MOD_SUB_VA_OVERWRITE, ctx, elem_addr, label, mod_id, CID_NVAL);
}

int bt_mesh_cfg_mod_sub_va_overwrite_vnd(struct bt_mesh_msg_ctx *ctx,
        u16_t elem_addr, const u8_t label[16],
        u16_t mod_id, u16_t cid)
{
    if (!ctx || !ctx->addr || !label || cid == CID_NVAL) {
        return -EINVAL;
    }
    return mod_sub_va(OP_MOD_SUB_VA_OVERWRITE, ctx, elem_addr, label, mod_id, cid);
}

static int mod_pub_get(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                       u16_t mod_id, u16_t cid)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 6 + 4);
    int err;

    bt_mesh_model_msg_init(msg, OP_MOD_PUB_GET);
    net_buf_simple_add_le16(msg, elem_addr);
    if (cid != CID_NVAL) {
        net_buf_simple_add_le16(msg, cid);
    }
    net_buf_simple_add_le16(msg, mod_id);

    err = bt_mesh_client_send_msg(cli->model, OP_MOD_PUB_GET, ctx, msg,
                                  timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_mod_pub_get(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr, u16_t mod_id)
{
    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }
    return mod_pub_get(ctx, elem_addr, mod_id, CID_NVAL);
}

int bt_mesh_cfg_mod_pub_get_vnd(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                                u16_t mod_id, u16_t cid)
{
    if (!ctx || !ctx->addr || cid == CID_NVAL) {
        return -EINVAL;
    }
    return mod_pub_get(ctx, elem_addr, mod_id, cid);
}

static int mod_pub_set(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                       u16_t mod_id, u16_t cid,
                       struct bt_mesh_cfg_mod_pub *pub)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 13 + 4);
    int err;

    bt_mesh_model_msg_init(msg, OP_MOD_PUB_SET);
    net_buf_simple_add_le16(msg, elem_addr);
    net_buf_simple_add_le16(msg, pub->addr);
    net_buf_simple_add_le16(msg, (pub->app_idx & (pub->cred_flag << 12)));
    net_buf_simple_add_u8(msg, pub->ttl);
    net_buf_simple_add_u8(msg, pub->period);
    net_buf_simple_add_u8(msg, pub->transmit);
    if (cid != CID_NVAL) {
        net_buf_simple_add_le16(msg, cid);
    }
    net_buf_simple_add_le16(msg, mod_id);

    err = bt_mesh_client_send_msg(cli->model, OP_MOD_PUB_SET, ctx, msg,
                                  timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_mod_pub_set(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                            u16_t mod_id, struct bt_mesh_cfg_mod_pub *pub)
{
    if (!ctx || !ctx->addr || !pub) {
        return -EINVAL;
    }
    return mod_pub_set(ctx, elem_addr, mod_id, CID_NVAL, pub);
}

int bt_mesh_cfg_mod_pub_set_vnd(struct bt_mesh_msg_ctx *ctx, u16_t elem_addr,
                                u16_t mod_id, u16_t cid,
                                struct bt_mesh_cfg_mod_pub *pub)
{
    if (!ctx || !ctx->addr || !pub || cid == CID_NVAL) {
        return -EINVAL;
    }
    return mod_pub_set(ctx, elem_addr, mod_id, cid, pub);
}

int bt_mesh_cfg_hb_sub_set(struct bt_mesh_msg_ctx *ctx,
                           struct bt_mesh_cfg_hb_sub *sub)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 5 + 4);
    int err;

    if (!ctx || !ctx->addr || !sub) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_HEARTBEAT_SUB_SET);
    net_buf_simple_add_le16(msg, sub->src);
    net_buf_simple_add_le16(msg, sub->dst);
    net_buf_simple_add_u8(msg, sub->period);

    err = bt_mesh_client_send_msg(cli->model, OP_HEARTBEAT_SUB_SET, ctx,
                                  msg, timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_hb_sub_get(struct bt_mesh_msg_ctx *ctx)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 0 + 4);
    int err;

    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_HEARTBEAT_SUB_GET);

    err = bt_mesh_client_send_msg(cli->model, OP_HEARTBEAT_SUB_GET, ctx,
                                  msg, timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_hb_pub_set(struct bt_mesh_msg_ctx *ctx,
                           const struct bt_mesh_cfg_hb_pub *pub)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 9 + 4);
    int err;

    if (!ctx || !ctx->addr || !pub) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_HEARTBEAT_PUB_SET);
    net_buf_simple_add_le16(msg, pub->dst);
    net_buf_simple_add_u8(msg, pub->count);
    net_buf_simple_add_u8(msg, pub->period);
    net_buf_simple_add_u8(msg, pub->ttl);
    net_buf_simple_add_le16(msg, pub->feat);
    net_buf_simple_add_le16(msg, pub->net_idx);

    err = bt_mesh_client_send_msg(cli->model, OP_HEARTBEAT_PUB_SET, ctx,
                                  msg, timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_hb_pub_get(struct bt_mesh_msg_ctx *ctx)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 0 + 4);
    int err;

    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_HEARTBEAT_PUB_GET);

    err = bt_mesh_client_send_msg(cli->model, OP_HEARTBEAT_PUB_GET, ctx,
                                  msg, timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

int bt_mesh_cfg_node_reset(struct bt_mesh_msg_ctx *ctx)
{
    struct net_buf_simple *msg = NET_BUF_SIMPLE(2 + 0 + 4);
    int err;

    if (!ctx || !ctx->addr) {
        return -EINVAL;
    }

    bt_mesh_model_msg_init(msg, OP_NODE_RESET);

    err = bt_mesh_client_send_msg(cli->model, OP_NODE_RESET, ctx, msg,
                                  timeout_handler, config_msg_timeout,
                                  true, NULL, NULL);
    if (err) {
        BT_ERR("%s: send failed (err %d)", __func__, err);
    }

    return err;
}

s32_t bt_mesh_cfg_cli_timeout_get(void)
{
    return config_msg_timeout;
}

void bt_mesh_cfg_cli_timeout_set(s32_t timeout)
{
    config_msg_timeout = timeout;
}

int bt_mesh_cfg_cli_init(struct bt_mesh_model *model, bool primary)
{
    bt_mesh_config_client_t *client   = NULL;
    config_internal_data_t  *internal = NULL;

    BT_DBG("primary %u", primary);

    if (!primary) {
        BT_ERR("Configuration Client only allowed in primary element");
        return -EINVAL;
    }

    if (!model) {
        BT_ERR("Configuration Client model is NULL");
        return -EINVAL;
    }

    client = (bt_mesh_config_client_t *)model->user_data;
    if (!client) {
        BT_ERR("No Configuration Client context provided");
        return -EINVAL;
    }

    /* TODO: call osi_free() when deinit function is invoked*/
    internal = osi_calloc(sizeof(config_internal_data_t));
    if (!internal) {
        BT_ERR("Allocate memory for Configuration Client internal data fail");
        return -ENOMEM;
    }

    sys_slist_init(&internal->queue);

    client->model         = model;
    client->op_pair_size  = ARRAY_SIZE(cfg_op_pair);
    client->op_pair       = cfg_op_pair;
    client->internal_data = internal;

    cli = client;

    /* Configuration Model security is device-key based */
    model->keys[0] = BT_MESH_KEY_DEV;

    return 0;
}

#endif /* #if CONFIG_BT_MESH */
