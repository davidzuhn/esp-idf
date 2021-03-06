/*
 * Copyright 2015-2018 Espressif Systems (Shanghai) PTE LTD
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <assert.h>
#include "sysinit/sysinit.h"
#include "nimble/hci_common.h"
#include "esp_nimble_hci.h"
#include "esp_bt.h"

static ble_hci_trans_rx_cmd_fn *ble_hci_rx_cmd_hs_cb;
static void *ble_hci_rx_cmd_hs_arg;

static ble_hci_trans_rx_cmd_fn *ble_hci_rx_cmd_ll_cb;
static void *ble_hci_rx_cmd_ll_arg;

static ble_hci_trans_rx_acl_fn *ble_hci_rx_acl_hs_cb;
static void *ble_hci_rx_acl_hs_arg;

static ble_hci_trans_rx_acl_fn *ble_hci_rx_acl_ll_cb;
static void *ble_hci_rx_acl_ll_arg;

static struct os_mbuf_pool ble_hci_acl_mbuf_pool;
static struct os_mempool_ext ble_hci_acl_pool;
/*
 * The MBUF payload size must accommodate the HCI data header size plus the
 * maximum ACL data packet length. The ACL block size is the size of the
 * mbufs we will allocate.
 */
#define ACL_BLOCK_SIZE  OS_ALIGN(MYNEWT_VAL(BLE_ACL_BUF_SIZE) \
        + BLE_MBUF_MEMBLOCK_OVERHEAD \
        + BLE_HCI_DATA_HDR_SZ, OS_ALIGNMENT)

static os_membuf_t ble_hci_acl_buf[
    OS_MEMPOOL_SIZE(MYNEWT_VAL(BLE_ACL_BUF_COUNT),
                    ACL_BLOCK_SIZE)];

static struct os_mempool ble_hci_cmd_pool;
static os_membuf_t ble_hci_cmd_buf[
    OS_MEMPOOL_SIZE(1, BLE_HCI_TRANS_CMD_SZ)
];

static struct os_mempool ble_hci_evt_hi_pool;
static os_membuf_t ble_hci_evt_hi_buf[
    OS_MEMPOOL_SIZE(MYNEWT_VAL(BLE_HCI_EVT_HI_BUF_COUNT),
                    MYNEWT_VAL(BLE_HCI_EVT_BUF_SIZE))
];

static struct os_mempool ble_hci_evt_lo_pool;
static os_membuf_t ble_hci_evt_lo_buf[
    OS_MEMPOOL_SIZE(MYNEWT_VAL(BLE_HCI_EVT_LO_BUF_COUNT),
                    MYNEWT_VAL(BLE_HCI_EVT_BUF_SIZE))
];

void
ble_hci_trans_cfg_hs(ble_hci_trans_rx_cmd_fn *cmd_cb,
                     void *cmd_arg,
                     ble_hci_trans_rx_acl_fn *acl_cb,
                     void *acl_arg)
{
    ble_hci_rx_cmd_hs_cb = cmd_cb;
    ble_hci_rx_cmd_hs_arg = cmd_arg;
    ble_hci_rx_acl_hs_cb = acl_cb;
    ble_hci_rx_acl_hs_arg = acl_arg;
}

void
ble_hci_trans_cfg_ll(ble_hci_trans_rx_cmd_fn *cmd_cb,
                     void *cmd_arg,
                     ble_hci_trans_rx_acl_fn *acl_cb,
                     void *acl_arg)
{
    ble_hci_rx_cmd_ll_cb = cmd_cb;
    ble_hci_rx_cmd_ll_arg = cmd_arg;
    ble_hci_rx_acl_ll_cb = acl_cb;
    ble_hci_rx_acl_ll_arg = acl_arg;
}

int
ble_hci_trans_hs_cmd_tx(uint8_t *cmd)
{
    uint16_t len;

    assert(cmd != NULL);
    *cmd = BLE_HCI_UART_H4_CMD;
    len = BLE_HCI_CMD_HDR_LEN + cmd[3] + 1;
    while (!esp_vhci_host_check_send_available()) {
    }
    esp_vhci_host_send_packet(cmd, len);

    ble_hci_trans_buf_free(cmd);
    return 0;
}

int
ble_hci_trans_ll_evt_tx(uint8_t *hci_ev)
{
    int rc;

    assert(ble_hci_rx_cmd_hs_cb != NULL);

    rc = ble_hci_rx_cmd_hs_cb(hci_ev, ble_hci_rx_cmd_hs_arg);
    return rc;
}

int
ble_hci_trans_hs_acl_tx(struct os_mbuf *om)
{
    uint16_t len = 0;
    uint8_t data[MYNEWT_VAL(BLE_ACL_BUF_SIZE) + 1];
    /* If this packet is zero length, just free it */
    if (OS_MBUF_PKTLEN(om) == 0) {
        os_mbuf_free_chain(om);
        return 0;
    }
    data[0] = BLE_HCI_UART_H4_ACL;
    len++;

    while (!esp_vhci_host_check_send_available()) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    os_mbuf_copydata(om, 0, OS_MBUF_PKTLEN(om), &data[1]);
    len += OS_MBUF_PKTLEN(om);

    esp_vhci_host_send_packet(data, len);

    os_mbuf_free_chain(om);

    return 0;
}

int
ble_hci_trans_ll_acl_tx(struct os_mbuf *om)
{
    int rc;

    assert(ble_hci_rx_acl_hs_cb != NULL);

    rc = ble_hci_rx_acl_hs_cb(om, ble_hci_rx_acl_hs_arg);
    return rc;
}

uint8_t *
ble_hci_trans_buf_alloc(int type)
{
    uint8_t *buf;

    switch (type) {
    case BLE_HCI_TRANS_BUF_CMD:
        buf = os_memblock_get(&ble_hci_cmd_pool);
        break;

    case BLE_HCI_TRANS_BUF_EVT_HI:
        buf = os_memblock_get(&ble_hci_evt_hi_pool);
        if (buf == NULL) {
            /* If no high-priority event buffers remain, try to grab a
             * low-priority one.
             */
            buf = ble_hci_trans_buf_alloc(BLE_HCI_TRANS_BUF_EVT_LO);
        }
        break;

    case BLE_HCI_TRANS_BUF_EVT_LO:
        buf = os_memblock_get(&ble_hci_evt_lo_pool);
        break;

    default:
        assert(0);
        buf = NULL;
    }

    return buf;
}

void
ble_hci_trans_buf_free(uint8_t *buf)
{
    int rc;
    /* XXX: this may look a bit odd, but the controller uses the command
    * buffer to send back the command complete/status as an immediate
    * response to the command. This was done to insure that the controller
    * could always send back one of these events when a command was received.
    * Thus, we check to see which pool the buffer came from so we can free
    * it to the appropriate pool
    */
    if (os_memblock_from(&ble_hci_evt_hi_pool, buf)) {
        rc = os_memblock_put(&ble_hci_evt_hi_pool, buf);
        assert(rc == 0);
    } else if (os_memblock_from(&ble_hci_evt_lo_pool, buf)) {
        rc = os_memblock_put(&ble_hci_evt_lo_pool, buf);
        assert(rc == 0);
    } else {
        assert(os_memblock_from(&ble_hci_cmd_pool, buf));
        rc = os_memblock_put(&ble_hci_cmd_pool, buf);
        assert(rc == 0);
    }
}

/**
 * Unsupported; the RAM transport does not have a dedicated ACL data packet
 * pool.
 */
int
ble_hci_trans_set_acl_free_cb(os_mempool_put_fn *cb, void *arg)
{
    return BLE_ERR_UNSUPPORTED;
}

int
ble_hci_trans_reset(void)
{
    /* No work to do.  All allocated buffers are owned by the host or
     * controller, and they will get freed by their owners.
     */
    return 0;
}

/**
 * Allocates a buffer (mbuf) for ACL operation.
 *
 * @return                      The allocated buffer on success;
 *                              NULL on buffer exhaustion.
 */
static struct os_mbuf *
ble_hci_trans_acl_buf_alloc(void)
{
    struct os_mbuf *m;
    uint8_t usrhdr_len;

#if MYNEWT_VAL(BLE_DEVICE)
    usrhdr_len = sizeof(struct ble_mbuf_hdr);
#elif MYNEWT_VAL(BLE_HS_FLOW_CTRL)
    usrhdr_len = BLE_MBUF_HS_HDR_LEN;
#else
    usrhdr_len = 0;
#endif

    m = os_mbuf_get_pkthdr(&ble_hci_acl_mbuf_pool, usrhdr_len);
    return m;
}

void
ble_hci_rx_acl(uint8_t *data, uint16_t len)
{
    struct os_mbuf *m;
    int sr;
    if (len < BLE_HCI_DATA_HDR_SZ || len > MYNEWT_VAL(BLE_ACL_BUF_SIZE)) {
        return;
    }

    m = ble_hci_trans_acl_buf_alloc();

    if (!m) {
        return;
    }
    if (os_mbuf_append(m, data, len)) {
        os_mbuf_free_chain(m);
        return;
    }
    OS_ENTER_CRITICAL(sr);
    ble_hci_rx_acl_hs_cb(m, NULL);
    OS_EXIT_CRITICAL(sr);
}

void ble_hci_transport_init(void)
{
    int rc;

    /* Ensure this function only gets called by sysinit. */
    SYSINIT_ASSERT_ACTIVE();


    rc = os_mempool_ext_init(&ble_hci_acl_pool,
                             MYNEWT_VAL(BLE_ACL_BUF_COUNT),
                             ACL_BLOCK_SIZE,
                             ble_hci_acl_buf,
                             "ble_hci_acl_pool");
    SYSINIT_PANIC_ASSERT(rc == 0);

    rc = os_mbuf_pool_init(&ble_hci_acl_mbuf_pool,
                           &ble_hci_acl_pool.mpe_mp,
                           ACL_BLOCK_SIZE,
                           MYNEWT_VAL(BLE_ACL_BUF_COUNT));
    SYSINIT_PANIC_ASSERT(rc == 0);

    /*
     * Create memory pool of HCI command buffers. NOTE: we currently dont
     * allow this to be configured. The controller will only allow one
     * outstanding command. We decided to keep this a pool in case we allow
     * allow the controller to handle more than one outstanding command.
     */
    rc = os_mempool_init(&ble_hci_cmd_pool,
                         1,
                         BLE_HCI_TRANS_CMD_SZ,
                         ble_hci_cmd_buf,
                         "ble_hci_cmd_pool");
    SYSINIT_PANIC_ASSERT(rc == 0);

    rc = os_mempool_init(&ble_hci_evt_hi_pool,
                         MYNEWT_VAL(BLE_HCI_EVT_HI_BUF_COUNT),
                         MYNEWT_VAL(BLE_HCI_EVT_BUF_SIZE),
                         ble_hci_evt_hi_buf,
                         "ble_hci_evt_hi_pool");
    SYSINIT_PANIC_ASSERT(rc == 0);

    rc = os_mempool_init(&ble_hci_evt_lo_pool,
                         MYNEWT_VAL(BLE_HCI_EVT_LO_BUF_COUNT),
                         MYNEWT_VAL(BLE_HCI_EVT_BUF_SIZE),
                         ble_hci_evt_lo_buf,
                         "ble_hci_evt_lo_pool");
    SYSINIT_PANIC_ASSERT(rc == 0);
}

/*
 * @brief: BT controller callback function, used to notify the upper layer that
 *         controller is ready to receive command
 */
static void controller_rcv_pkt_ready(void)
{
}

/*
 * @brief: BT controller callback function, to transfer data packet to the host
 */
static int host_rcv_pkt(uint8_t *data, uint16_t len)
{

    if (data[0] == BLE_HCI_UART_H4_EVT) {
        uint8_t *evbuf;
        int totlen;
        int rc;

        totlen = BLE_HCI_EVENT_HDR_LEN + data[2];
        assert(totlen <= UINT8_MAX + BLE_HCI_EVENT_HDR_LEN);

        evbuf = ble_hci_trans_buf_alloc(
                    BLE_HCI_TRANS_BUF_EVT_HI);
        assert(evbuf != NULL);

        if (data[1] == BLE_HCI_EVCODE_HW_ERROR) {
            assert(0);
        }
        memcpy(evbuf, &data[1], totlen);

        if (1 /*os_started()*/) {
            rc = ble_hci_trans_ll_evt_tx(evbuf);
        } else {
            //rc = ble_hs_hci_evt_process(evbuf);
        }

        assert(rc == 0);
    } else if (data[0] == BLE_HCI_UART_H4_ACL) {
        extern void ble_hci_rx_acl(uint8_t *data, uint16_t len);

        ble_hci_rx_acl(data + 1, len - 1);
    }
    return 0;
}

static esp_vhci_host_callback_t vhci_host_cb = {
    controller_rcv_pkt_ready,
    host_rcv_pkt
};

esp_err_t esp_nimble_hci_init(void)
{
    esp_err_t ret;
    if ((ret = esp_vhci_host_register_callback(&vhci_host_cb)) != ESP_OK) {
        return ret;
    }

    ble_hci_transport_init();

    return ESP_OK;
}

esp_err_t esp_nimble_hci_and_controller_init(void)
{
    esp_err_t ret;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret) {
        return ret;
    }

    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        return ret;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_BLE)) != ESP_OK) {
        return ret;
    }

    return esp_nimble_hci_init();
}
