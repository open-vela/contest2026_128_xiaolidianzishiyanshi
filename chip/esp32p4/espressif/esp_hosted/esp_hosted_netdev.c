/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/esp_hosted_netdev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <sched.h>
#include <syslog.h>

#include <arpa/inet.h>
#include <net/if_arp.h>
#include <nuttx/wireless/wireless.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/kmalloc.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>

#include "esp_hosted_port.h"
#include "esp_hosted_interface.h"
#include "esp_hosted_netdev.h"
#include "esp_hosted.h"
#include "esp_hosted_api_priv.h"
#include "esp_hosted_api_types.h"
#include "esp_wifi_remote.h"
#include "transport_drv.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A full Ethernet frame occupies about eight 196-byte IOBs.  Keep the
 * driver's in-flight quota below half of the 320-entry system IOB pool so
 * TCP control traffic and the transmit path cannot be starved by RX bursts.
 */

#define RX_BUF_COUNT    16
#define TX_BUF_COUNT    1
#define CONNECT_TIMEOUT 30
#define HOSTED_SCAN_RESULT_SIZE 4096
#define HOSTED_SCAN_MAX_APS     64
#define HOSTED_MAC_LEN 6
#define HOSTED_IW_EVENT_SIZE(field) \
  (offsetof(struct iw_event, u) + sizeof(((struct iw_event *)0)->u.field))

#define HOSTED_WIFI_11B_MAX_BITRATE       11
#define HOSTED_WIFI_11G_MAX_BITRATE       54
#define HOSTED_WIFI_11N_MCS7_HT20_BITRATE 72
#define HOSTED_WIFI_11N_MCS7_HT40_BITRATE 150

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_hosted_priv_s
{
  /* Upper-half interface */

  struct netdev_lowerhalf_s dev;

  /* Lower-half data */

  bool initialized;
  bool started;
  uint32_t mode;
  wifi_config_t wifi_cfg;
  uint8_t *scan_result;
  size_t scan_result_size;

  spinlock_t rx_lock;
  netpkt_queue_t netdev_rx_queue;
  uint8_t flatbuf[CONFIG_NET_ETH_PKTSIZE];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Netdev operations */

static int hosted_ifup(struct netdev_lowerhalf_s *dev);
static int hosted_ifdown(struct netdev_lowerhalf_s *dev);
static int hosted_transmit(struct netdev_lowerhalf_s *dev, netpkt_t *pkt);
static netpkt_t *hosted_receive(struct netdev_lowerhalf_s *dev);
static void hosted_reclaim(struct netdev_lowerhalf_s *dev);
static int hosted_ioctl(struct netdev_lowerhalf_s *dev, int cmd,
                        unsigned long arg);

/* Wireless operations */

static int hosted_connect(struct netdev_lowerhalf_s *dev);
static int hosted_disconnect(struct netdev_lowerhalf_s *dev);
static int hosted_essid(struct netdev_lowerhalf_s *dev,
                        struct iwreq *iwr, bool set);
static int hosted_bssid(struct netdev_lowerhalf_s *dev,
                        struct iwreq *iwr, bool set);
static int hosted_passwd(struct netdev_lowerhalf_s *dev,
                         struct iwreq *iwr, bool set);
static int hosted_mode(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set);
static int hosted_auth(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set);
static int hosted_freq(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set);
static int hosted_bitrate(struct netdev_lowerhalf_s *dev,
                          struct iwreq *iwr, bool set);
static int hosted_txpower(struct netdev_lowerhalf_s *dev,
                          struct iwreq *iwr, bool set);
static int hosted_country(struct netdev_lowerhalf_s *dev,
                          struct iwreq *iwr, bool set);
static int hosted_sensitivity(struct netdev_lowerhalf_s *dev,
                              struct iwreq *iwr, bool set);
static int hosted_scan(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set);
static int hosted_range(struct netdev_lowerhalf_s *dev,
                        struct iwreq *iwr);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Netdev operations */

static const struct netdev_ops_s g_netdev_ops =
{
    .ifup = hosted_ifup,
    .ifdown = hosted_ifdown,
    .transmit = hosted_transmit,
    .receive = hosted_receive,
#ifdef CONFIG_NETDEV_IOCTL
    .ioctl = hosted_ioctl,
#endif
    .reclaim = hosted_reclaim,
};

/* Wireless operations */

static const struct wireless_ops_s g_wireless_ops =
{
  .connect = hosted_connect,
  .disconnect = hosted_disconnect,
  .essid = hosted_essid,
  .bssid = hosted_bssid,
  .passwd = hosted_passwd,
  .mode = hosted_mode,
  .auth = hosted_auth,
  .freq = hosted_freq,
  .bitrate = hosted_bitrate,
  .txpower = hosted_txpower,
  .country = hosted_country,
  .sensitivity = hosted_sensitivity,
  .scan = hosted_scan,
  .range = hosted_range,
};

/* Station interface control structure */

static struct esp_hosted_priv_s g_hosted_sta =
{
  .dev =
  {
    .ops = &g_netdev_ops,
    .iw_ops = &g_wireless_ops,
  },
  .initialized = false,
  .mode = IW_MODE_INFRA,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int hosted_esp_to_errno(esp_err_t err)
{
  switch (err)
    {
      case ESP_OK:
        return OK;
      case ESP_ERR_NO_MEM:
        return -ENOMEM;
      case ESP_ERR_INVALID_ARG:
      case ESP_ERR_INVALID_SIZE:
        return -EINVAL;
      case ESP_ERR_INVALID_STATE:
        return -EIO;
      case ESP_ERR_NOT_FOUND:
        return -ENOENT;
      case ESP_ERR_NOT_SUPPORTED:
        return -EOPNOTSUPP;
      case ESP_ERR_TIMEOUT:
        return -ETIMEDOUT;
      default:
        return -EIO;
    }
}

static int hosted_apply_sta_config(struct esp_hosted_priv_s *priv)
{
  esp_err_t ret;

  if (!priv->initialized)
    {
      return -ENETDOWN;
    }

  ret = esp_wifi_remote_set_config(WIFI_IF_STA, &priv->wifi_cfg);
  if (ret != ESP_OK)
    {
      syslog(LOG_ERR,
             "ESP-Hosted: set station config failed: %d (0x%x)\n",
             ret, (unsigned int)ret);
    }

  return hosted_esp_to_errno(ret);
}

static uint8_t hosted_freq_to_channel(const struct iw_freq *freq)
{
  long mhz;

  if (freq->e == 0 && freq->m >= 1 && freq->m <= 14)
    {
      return freq->m;
    }

  if (freq->e == 6)
    {
      mhz = freq->m;
    }
  else if (freq->e == 0 && freq->m >= 2400 && freq->m <= 2500)
    {
      mhz = freq->m;
    }
  else
    {
      return 0;
    }

  if (mhz == 2484)
    {
      return 14;
    }

  if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0)
    {
      return (mhz - 2407) / 5;
    }

  return 0;
}

static int hosted_scan_append(struct esp_hosted_priv_s *priv,
                              uint16_t cmd, const union iwreq_data *data,
                              size_t data_size, const void *extra,
                              size_t extra_size)
{
  struct iw_event *iwe;
  size_t aligned_extra = (extra_size + 3) & ~3;
  size_t event_size = offsetof(struct iw_event, u) + data_size + aligned_extra;

  if (priv->scan_result_size + event_size > HOSTED_SCAN_RESULT_SIZE)
    {
      return -ENOSPC;
    }

  iwe = (struct iw_event *)&priv->scan_result[priv->scan_result_size];
  memset(iwe, 0, event_size);
  iwe->len = event_size;
  iwe->cmd = cmd;
  memcpy(&iwe->u, data, data_size);

  if (extra_size > 0)
    {
      iwe->u.data.pointer = (void *)data_size;
      memcpy((uint8_t *)&iwe->u + data_size, extra, extra_size);
    }

  priv->scan_result_size += event_size;
  return OK;
}

static int hosted_build_scan_results(struct esp_hosted_priv_s *priv,
                                     wifi_ap_record_t *records,
                                     uint16_t count)
{
  union iwreq_data data;
  size_t essid_len;
  uint16_t i;
  int ret = OK;

  if (priv->scan_result == NULL)
    {
      priv->scan_result = kmm_zalloc(HOSTED_SCAN_RESULT_SIZE);
      if (priv->scan_result == NULL)
        {
          return -ENOMEM;
        }
    }
  else
    {
      memset(priv->scan_result, 0, HOSTED_SCAN_RESULT_SIZE);
    }

  priv->scan_result_size = 0;

  for (i = 0; i < count; i++)
    {
      memset(&data, 0, sizeof(data));
      data.ap_addr.sa_family = ARPHRD_ETHER;
      memcpy(data.ap_addr.sa_data, records[i].bssid, HOSTED_MAC_LEN);
      ret = hosted_scan_append(priv, SIOCGIWAP, &data,
                               sizeof(data.ap_addr), NULL, 0);
      if (ret < 0)
        {
          break;
        }

      essid_len = strnlen((char *)records[i].ssid, IW_ESSID_MAX_SIZE);
      memset(&data, 0, sizeof(data));
      data.essid.length = essid_len;
      ret = hosted_scan_append(priv, SIOCGIWESSID, &data,
                               sizeof(data.essid), records[i].ssid,
                               essid_len);
      if (ret < 0)
        {
          break;
        }

      memset(&data, 0, sizeof(data));
      data.qual.level = records[i].rssi;
      data.qual.updated = IW_QUAL_DBM | IW_QUAL_ALL_UPDATED;
      ret = hosted_scan_append(priv, IWEVQUAL, &data,
                               sizeof(data.qual), NULL, 0);
      if (ret < 0)
        {
          break;
        }

      memset(&data, 0, sizeof(data));
      data.mode = IW_MODE_MASTER;
      ret = hosted_scan_append(priv, SIOCGIWMODE, &data,
                               sizeof(data.mode), NULL, 0);
      if (ret < 0)
        {
          break;
        }

      memset(&data, 0, sizeof(data));
      data.data.flags = records[i].authmode == WIFI_AUTH_OPEN ?
                        IW_ENCODE_DISABLED :
                        IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
      ret = hosted_scan_append(priv, SIOCGIWENCODE, &data,
                               sizeof(data.data), NULL, 0);
      if (ret < 0)
        {
          break;
        }

      memset(&data, 0, sizeof(data));
      data.freq.m = records[i].primary;
      data.freq.e = 0;
      ret = hosted_scan_append(priv, SIOCGIWFREQ, &data,
                               sizeof(data.freq), NULL, 0);
      if (ret < 0)
        {
          break;
        }
    }

  return ret == -ENOSPC ? OK : ret;
}

/****************************************************************************
 * Name: hosted_ifup
 *
 * Description:
 *   Bring up the network device.
 *
 ****************************************************************************/

static int hosted_ifup(struct netdev_lowerhalf_s *dev)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  struct net_driver_s *netdev = &priv->dev.netdev;
  irqstate_t flags;
  int ret;

#ifdef CONFIG_NET_IPv4
  wlinfo("Bringing up: %u.%u.%u.%u\n",
        ip4_addr1(netdev->d_ipaddr), ip4_addr2(netdev->d_ipaddr),
        ip4_addr3(netdev->d_ipaddr), ip4_addr4(netdev->d_ipaddr));
#endif

  /* Clear RX queue */

  flags = spin_lock_irqsave(&priv->rx_lock);
  netpkt_free_queue(&priv->netdev_rx_queue);
  spin_unlock_irqrestore(&priv->rx_lock, flags);

  if (priv->scan_result != NULL)
    {
      kmm_free(priv->scan_result);
      priv->scan_result = NULL;
      priv->scan_result_size = 0;
    }

  /* The transport is initialized during board bring-up.  Initialize the
   * remote Wi-Fi driver lazily so the coprocessor boot event has time to
   * arrive before the first synchronous RPC request.
   */

  if (!priv->initialized)
    {
      wifi_init_config_t cfg;
      uint8_t mac[6];

      memset(&cfg, 0, sizeof(cfg));

      /* These are the ESP-IDF defaults used by the ESP32-C6 coprocessor.
       * Only the scalar portion of wifi_init_config_t is serialized by
       * ESP-Hosted; the OS and crypto function tables remain host-local.
       * Do not leave scalar fields at zero merely because they are not used
       * by NuttX: the coprocessor validates them before esp_wifi_init().
       */

      cfg.static_rx_buf_num = 10;
      cfg.dynamic_rx_buf_num = 32;
      cfg.tx_buf_type = 1;
      cfg.static_tx_buf_num = 16;
      cfg.dynamic_tx_buf_num = 32;
      cfg.rx_mgmt_buf_type = 0;
      cfg.rx_mgmt_buf_num = 5;
      cfg.cache_tx_buf_num = 0;
      cfg.csi_enable = 0;
      cfg.ampdu_rx_enable = 1;
      cfg.ampdu_tx_enable = 1;
      cfg.amsdu_tx_enable = 0;
      cfg.nvs_enable = 1;
      cfg.nano_enable = 0;
      cfg.rx_ba_win = 6;
      cfg.wifi_task_core_id = 0;
      cfg.beacon_max_len = 752;
      cfg.mgmt_sbuf_num = 32;
      cfg.feature_caps = 0;
      cfg.sta_disconnected_pm = true;
      cfg.espnow_max_encrypt_num = 7;
      cfg.tx_hetb_queue_num = 3;
      cfg.dump_hesigb_enable = false;
      cfg.magic = WIFI_INIT_CONFIG_MAGIC;

      ret = esp_wifi_remote_init(&cfg);
      if (ret != ESP_OK)
        {
          syslog(LOG_ERR,
                 "ESP-Hosted: Remote Wi-Fi init failed: %d (0x%x)\n",
                 ret, (unsigned int)ret);
          return -EIO;
        }

      syslog(LOG_INFO, "ESP-Hosted: Remote Wi-Fi init succeeded\n");

      ret = esp_wifi_remote_set_mode(WIFI_MODE_STA);
      if (ret != ESP_OK)
        {
          syslog(LOG_ERR,
                 "ESP-Hosted: Remote Wi-Fi mode setup failed: %d (0x%x)\n",
                 ret, (unsigned int)ret);
          return -EIO;
        }

      syslog(LOG_INFO, "ESP-Hosted: Remote Wi-Fi station mode selected\n");

      ret = esp_wifi_remote_get_mac(WIFI_IF_STA, mac);
      if (ret == ESP_OK)
        {
          memcpy(dev->netdev.d_mac.ether.ether_addr_octet, mac, sizeof(mac));
          syslog(LOG_INFO,
                 "ESP-Hosted: Remote Wi-Fi MAC: "
                 "%02x:%02x:%02x:%02x:%02x:%02x\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
      else
        {
          syslog(LOG_ERR,
                 "ESP-Hosted: Remote Wi-Fi MAC query failed: %d (0x%x)\n",
                 ret, (unsigned int)ret);
          return -EIO;
        }

      priv->initialized = true;
    }

  if (!priv->started)
    {
      ret = esp_wifi_remote_start();
      if (ret != ESP_OK)
        {
          syslog(LOG_ERR,
                 "ESP-Hosted: Remote Wi-Fi start failed: %d (0x%x)\n",
                 ret, (unsigned int)ret);
          return -EIO;
        }

      priv->started = true;
      syslog(LOG_INFO, "ESP-Hosted: Remote Wi-Fi started\n");
    }

  return OK;
}

/****************************************************************************
 * Name: hosted_ifdown
 *
 * Description:
 *   Bring down the network device.
 *
 ****************************************************************************/

static int hosted_ifdown(struct netdev_lowerhalf_s *dev)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  irqstate_t flags;

  /* Clear RX queue */

  flags = spin_lock_irqsave(&priv->rx_lock);
  netpkt_free_queue(&priv->netdev_rx_queue);
  spin_unlock_irqrestore(&priv->rx_lock, flags);

  if (priv->scan_result != NULL)
    {
      kmm_free(priv->scan_result);
      priv->scan_result = NULL;
      priv->scan_result_size = 0;
    }

  if (priv->started)
    {
      esp_wifi_remote_disconnect();
      esp_wifi_remote_stop();
      priv->started = false;
    }

  netdev_lower_carrier_off(dev);

  return OK;
}

/****************************************************************************
 * Name: hosted_transmit
 *
 * Description:
 *   Transmit function required by the netdev ops.
 *
 ****************************************************************************/

static int hosted_transmit(struct netdev_lowerhalf_s *dev, netpkt_t *pkt)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  unsigned int len = netpkt_getdatalen(dev, pkt);
  int ret;

  if (len > sizeof(priv->flatbuf))
    {
      return -EMSGSIZE;
    }

  /* Copy data from the packet to the flat buffer */

  netpkt_copyout(dev, priv->flatbuf, pkt, len, 0);

  /* Send via ESP-Hosted */

  ret = esp_wifi_remote_channel_tx(WIFI_IF_STA, priv->flatbuf, len);
  if (ret != ESP_OK) {
    wlerr("ERROR: Failed to transmit packet: %d\n", ret);
    return -EIO;
  }

  /* Free the packet after sending */

  netpkt_free(dev, pkt, NETPKT_TX);
  netdev_lower_txdone(dev);

  return OK;
}

/****************************************************************************
 * Name: hosted_receive
 *
 * Description:
 *   Receive function required by the netdev ops.
 *
 ****************************************************************************/

static netpkt_t *hosted_receive(struct netdev_lowerhalf_s *dev)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  irqstate_t flags;
  netpkt_t *pkt;

  flags = spin_lock_irqsave(&priv->rx_lock);
  pkt = netpkt_remove_queue(&priv->netdev_rx_queue);
  spin_unlock_irqrestore(&priv->rx_lock, flags);

  return pkt;
}

/****************************************************************************
 * Name: hosted_ioctl
 *
 * Description:
 *   Ioctl function required by the netdev ops.
 *
 ****************************************************************************/

static int hosted_ioctl(struct netdev_lowerhalf_s *dev, int cmd,
                        unsigned long arg)
{
  struct iwreq *iwr = (struct iwreq *)(uintptr_t)arg;
  wifi_ps_type_t ps;
  esp_err_t ret;

  (void)dev;

  if (iwr == NULL)
    {
      return -EINVAL;
    }

  switch (cmd)
    {
      case SIOCSIWPWSAVE:
        ps = iwr->u.power.flags ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE;
        return hosted_esp_to_errno(esp_wifi_remote_set_ps(ps));

      case SIOCGIWPWSAVE:
        ret = esp_wifi_remote_get_ps(&ps);
        if (ret == ESP_OK)
          {
            iwr->u.power.flags = ps != WIFI_PS_NONE;
          }

        return hosted_esp_to_errno(ret);

      case SIOCSIWPOWER:
        ps = iwr->u.power.disabled ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM;
        return hosted_esp_to_errno(esp_wifi_remote_set_ps(ps));

      case SIOCGIWPOWER:
        ret = esp_wifi_remote_get_ps(&ps);
        if (ret == ESP_OK)
          {
            iwr->u.power.disabled = ps == WIFI_PS_NONE;
          }

        return hosted_esp_to_errno(ret);

      default:
        break;
    }

  return -ENOTTY;
}

/****************************************************************************
 * Name: hosted_reclaim
 *
 * Description:
 *   Reclaim function required by the netdev ops.
 *
 ****************************************************************************/

static void hosted_reclaim(struct netdev_lowerhalf_s *dev)
{
  /* Nothing to reclaim */
}

/****************************************************************************
 * Name: hosted_connect
 *
 * Description:
 *   Connect to a Wi-Fi network.
 *
 ****************************************************************************/

static int hosted_connect(struct netdev_lowerhalf_s *dev)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  int timeout_count;
  int ret;

  if (!priv->started)
    {
      return -ENETDOWN;
    }

  ret = hosted_apply_sta_config(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_wifi_remote_connect();
  if (ret != ESP_OK)
    {
      return hosted_esp_to_errno(ret);
    }

  for (timeout_count = 0; timeout_count < CONNECT_TIMEOUT; timeout_count++)
    {
      if ((dev->netdev.d_flags & IFF_RUNNING) != 0)
        {
          return OK;
        }

      nxsig_usleep(USEC_PER_SEC);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: hosted_disconnect
 *
 * Description:
 *   Disconnect from a Wi-Fi network.
 *
 ****************************************************************************/

static int hosted_disconnect(struct netdev_lowerhalf_s *dev)
{
  esp_err_t ret;

  ret = esp_wifi_remote_disconnect();
  if (ret == ESP_OK)
    {
      netdev_lower_carrier_off(dev);
    }

  return hosted_esp_to_errno(ret);
}

/****************************************************************************
 * Name: hosted_essid
 *
 * Description:
 *   Set/get the ESSID.
 *
 ****************************************************************************/

static int hosted_essid(struct netdev_lowerhalf_s *dev,
                        struct iwreq *iwr, bool set)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  struct iw_point *essid = &iwr->u.essid;
  size_t len;

  if (set)
    {
      if (essid->pointer == NULL || essid->length > sizeof(priv->wifi_cfg.sta.ssid))
        {
          return -EINVAL;
        }

      memset(priv->wifi_cfg.sta.ssid, 0, sizeof(priv->wifi_cfg.sta.ssid));
      memcpy(priv->wifi_cfg.sta.ssid, essid->pointer, essid->length);
      memset(priv->wifi_cfg.sta.sae_h2e_identifier, 0,
             sizeof(priv->wifi_cfg.sta.sae_h2e_identifier));
      priv->wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
      return priv->initialized ? hosted_apply_sta_config(priv) : OK;
    }

  len = strnlen((char *)priv->wifi_cfg.sta.ssid,
                sizeof(priv->wifi_cfg.sta.ssid));
  if (essid->pointer == NULL || essid->length < len)
    {
      essid->length = len;
      return -E2BIG;
    }

  memcpy(essid->pointer, priv->wifi_cfg.sta.ssid, len);
  essid->length = len;
  essid->flags = IW_ESSID_ON;
  return OK;
}

static int hosted_bssid(struct netdev_lowerhalf_s *dev,
                        struct iwreq *iwr, bool set)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  struct sockaddr *addr = &iwr->u.ap_addr;
  const uint8_t broadcast[HOSTED_MAC_LEN] =
    {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  wifi_ap_record_t ap_info;

  if (set)
    {
      priv->wifi_cfg.sta.bssid_set =
        memcmp(addr->sa_data, broadcast, HOSTED_MAC_LEN) != 0;
      if (priv->wifi_cfg.sta.bssid_set)
        {
          memcpy(priv->wifi_cfg.sta.bssid, addr->sa_data, HOSTED_MAC_LEN);
        }
      else
        {
          memset(priv->wifi_cfg.sta.bssid, 0, HOSTED_MAC_LEN);
        }

      return priv->initialized ? hosted_apply_sta_config(priv) : OK;
    }

  addr->sa_family = ARPHRD_ETHER;
  if (priv->initialized &&
      esp_wifi_remote_sta_get_ap_info(&ap_info) == ESP_OK)
    {
      memcpy(addr->sa_data, ap_info.bssid, HOSTED_MAC_LEN);
    }
  else
    {
      memcpy(addr->sa_data, priv->wifi_cfg.sta.bssid, HOSTED_MAC_LEN);
    }

  return OK;
}

/****************************************************************************
 * Name: hosted_passwd
 *
 * Description:
 *   Set/get the Wi-Fi password.
 *
 ****************************************************************************/

static int hosted_passwd(struct netdev_lowerhalf_s *dev,
                         struct iwreq *iwr, bool set)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  struct iw_encode_ext *ext = iwr->u.encoding.pointer;
  wifi_ap_record_t ap_info;
  size_t len;

  if (ext == NULL)
    {
      return -EINVAL;
    }

  if (set)
    {
      if (ext->key_len > sizeof(priv->wifi_cfg.sta.password))
        {
          return -EINVAL;
        }

      memset(priv->wifi_cfg.sta.password, 0,
             sizeof(priv->wifi_cfg.sta.password));
      if (ext->alg != IW_ENCODE_ALG_NONE)
        {
          memcpy(priv->wifi_cfg.sta.password, ext->key, ext->key_len);
        }

      return priv->initialized ? hosted_apply_sta_config(priv) : OK;
    }

  len = strnlen((char *)priv->wifi_cfg.sta.password,
                sizeof(priv->wifi_cfg.sta.password));
  if (iwr->u.encoding.length < sizeof(*ext) + len)
    {
      iwr->u.encoding.length = sizeof(*ext) + len;
      return -E2BIG;
    }

  ext->key_len = len;
  memcpy(ext->key, priv->wifi_cfg.sta.password, len);

  if (esp_wifi_remote_sta_get_ap_info(&ap_info) == ESP_OK)
    {
      switch (ap_info.pairwise_cipher)
        {
          case WIFI_CIPHER_TYPE_NONE:
            ext->alg = IW_ENCODE_ALG_NONE;
            break;
          case WIFI_CIPHER_TYPE_WEP40:
          case WIFI_CIPHER_TYPE_WEP104:
            ext->alg = IW_ENCODE_ALG_WEP;
            break;
          case WIFI_CIPHER_TYPE_TKIP:
            ext->alg = IW_ENCODE_ALG_TKIP;
            break;
          case WIFI_CIPHER_TYPE_CCMP:
          case WIFI_CIPHER_TYPE_TKIP_CCMP:
            ext->alg = IW_ENCODE_ALG_CCMP;
            break;
          case WIFI_CIPHER_TYPE_AES_CMAC128:
            ext->alg = IW_ENCODE_ALG_AES_CMAC;
            break;
          default:
            return -EOPNOTSUPP;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: hosted_mode
 *
 * Description:
 *   Set/get the Wi-Fi mode.
 *
 ****************************************************************************/

static int hosted_mode(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;

  if (set && iwr->u.mode != IW_MODE_INFRA)
    {
      return -EOPNOTSUPP;
    }

  priv->mode = IW_MODE_INFRA;
  iwr->u.mode = priv->mode;
  return OK;
}

/****************************************************************************
 * Name: hosted_auth
 *
 * Description:
 *   Set/get the authentication mode.
 *
 ****************************************************************************/

static int hosted_auth(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  wifi_ap_record_t ap_info;
  uint32_t cipher;
  int index = iwr->u.param.flags & IW_AUTH_INDEX;

  if (set)
    {
      switch (index)
        {
          case IW_AUTH_WPA_VERSION:
            switch (iwr->u.param.value)
              {
                case IW_AUTH_WPA_VERSION_DISABLED:
                  priv->wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
                  break;
                case IW_AUTH_WPA_VERSION_WPA:
                  priv->wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
                  break;
                case IW_AUTH_WPA_VERSION_WPA2:
                  priv->wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
                  break;
                case IW_AUTH_WPA_VERSION_WPA3:
                  priv->wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA3_PSK;
                  break;
                default:
                  return -EINVAL;
              }
            break;

          case IW_AUTH_CIPHER_PAIRWISE:
          case IW_AUTH_CIPHER_GROUP:
            switch (iwr->u.param.value)
              {
                case IW_AUTH_CIPHER_NONE:
                  priv->wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
                  break;
                case IW_AUTH_CIPHER_WEP40:
                case IW_AUTH_CIPHER_WEP104:
                  priv->wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WEP;
                  break;
                case IW_AUTH_CIPHER_TKIP:
                case IW_AUTH_CIPHER_CCMP:
                case IW_AUTH_CIPHER_AES_CMAC:
                  break;
                default:
                  return -EINVAL;
              }
            break;

          default:
            return -EOPNOTSUPP;
        }

      return priv->initialized ? hosted_apply_sta_config(priv) : OK;
    }

  if (index == IW_AUTH_CIPHER_PAIRWISE || index == IW_AUTH_CIPHER_GROUP)
    {
      if (esp_wifi_remote_sta_get_ap_info(&ap_info) != ESP_OK)
        {
          return -ENOTCONN;
        }

      cipher = index == IW_AUTH_CIPHER_PAIRWISE ?
               ap_info.pairwise_cipher : ap_info.group_cipher;
      switch (cipher)
        {
          case WIFI_CIPHER_TYPE_NONE:
            iwr->u.param.value = IW_AUTH_CIPHER_NONE;
            break;
          case WIFI_CIPHER_TYPE_WEP40:
            iwr->u.param.value = IW_AUTH_CIPHER_WEP40;
            break;
          case WIFI_CIPHER_TYPE_WEP104:
            iwr->u.param.value = IW_AUTH_CIPHER_WEP104;
            break;
          case WIFI_CIPHER_TYPE_TKIP:
            iwr->u.param.value = IW_AUTH_CIPHER_TKIP;
            break;
          case WIFI_CIPHER_TYPE_CCMP:
          case WIFI_CIPHER_TYPE_TKIP_CCMP:
            iwr->u.param.value = IW_AUTH_CIPHER_CCMP;
            break;
          case WIFI_CIPHER_TYPE_AES_CMAC128:
            iwr->u.param.value = IW_AUTH_CIPHER_AES_CMAC;
            break;
          default:
            return -EOPNOTSUPP;
        }

      return OK;
    }

  if (index == IW_AUTH_WPA_VERSION)
    {
      switch (priv->wifi_cfg.sta.threshold.authmode)
        {
          case WIFI_AUTH_OPEN:
            iwr->u.param.value = IW_AUTH_WPA_VERSION_DISABLED;
            break;
          case WIFI_AUTH_WPA_PSK:
            iwr->u.param.value = IW_AUTH_WPA_VERSION_WPA;
            break;
          case WIFI_AUTH_WPA3_PSK:
          case WIFI_AUTH_WPA2_WPA3_PSK:
            iwr->u.param.value = IW_AUTH_WPA_VERSION_WPA3;
            break;
          default:
            iwr->u.param.value = IW_AUTH_WPA_VERSION_WPA2;
            break;
        }

      return OK;
    }

  return -EOPNOTSUPP;
}

/****************************************************************************
 * Name: hosted_freq
 *
 * Description:
 *   Set/get the frequency.
 *
 ****************************************************************************/

static int hosted_freq(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  wifi_ap_record_t ap_info;
  uint8_t channel;
  esp_err_t ret;

  if (set)
    {
      if (iwr->u.freq.flags != IW_FREQ_FIXED)
        {
          priv->wifi_cfg.sta.channel = 0;
          return priv->initialized ? hosted_apply_sta_config(priv) : OK;
        }

      channel = hosted_freq_to_channel(&iwr->u.freq);
      if (channel == 0)
        {
          return -EINVAL;
        }

      priv->wifi_cfg.sta.channel = channel;
      return priv->initialized ? hosted_apply_sta_config(priv) : OK;
    }

  if (priv->initialized &&
      esp_wifi_remote_sta_get_ap_info(&ap_info) == ESP_OK)
    {
      channel = ap_info.primary;
    }
  else
    {
      channel = priv->wifi_cfg.sta.channel;
    }

  if (channel == 0 && priv->initialized)
    {
      wifi_second_chan_t second;
      ret = esp_wifi_remote_get_channel(&channel, &second);
      if (ret != ESP_OK)
        {
          return hosted_esp_to_errno(ret);
        }
    }

  iwr->u.freq.flags = IW_FREQ_FIXED;
  iwr->u.freq.e = 0;
  iwr->u.freq.m = channel == 14 ? 2484 : 2407 + 5 * channel;

  return OK;
}

static int hosted_bitrate(struct netdev_lowerhalf_s *dev,
                          struct iwreq *iwr, bool set)
{
  wifi_ap_record_t ap_info;
  esp_err_t ret;

  (void)dev;
  if (set)
    {
      return -EOPNOTSUPP;
    }

  ret = esp_wifi_remote_sta_get_ap_info(&ap_info);
  if (ret != ESP_OK)
    {
      return hosted_esp_to_errno(ret);
    }

  iwr->u.bitrate.fixed = 1;
  if (ap_info.phy_11n)
    {
      iwr->u.bitrate.value = ap_info.second ?
        HOSTED_WIFI_11N_MCS7_HT40_BITRATE :
        HOSTED_WIFI_11N_MCS7_HT20_BITRATE;
    }
  else if (ap_info.phy_11g)
    {
      iwr->u.bitrate.value = HOSTED_WIFI_11G_MAX_BITRATE;
    }
  else if (ap_info.phy_11b)
    {
      iwr->u.bitrate.value = HOSTED_WIFI_11B_MAX_BITRATE;
    }
  else
    {
      return -EIO;
    }

  return OK;
}

static int hosted_txpower(struct netdev_lowerhalf_s *dev,
                          struct iwreq *iwr, bool set)
{
  int8_t power;
  esp_err_t ret;

  (void)dev;
  if (set)
    {
      if (iwr->u.txpower.flags == IW_TXPOW_RELATIVE)
        {
          power = iwr->u.txpower.value;
        }
      else if (iwr->u.txpower.flags == IW_TXPOW_DBM)
        {
          power = iwr->u.txpower.value * 4;
        }
      else
        {
          return -EOPNOTSUPP;
        }

      if (power < 8 || power > 84)
        {
          return -ERANGE;
        }

      return hosted_esp_to_errno(esp_wifi_remote_set_max_tx_power(power));
    }

  ret = esp_wifi_remote_get_max_tx_power(&power);
  if (ret != ESP_OK)
    {
      return hosted_esp_to_errno(ret);
    }

  iwr->u.txpower.disabled = 0;
  iwr->u.txpower.flags = IW_TXPOW_DBM;
  iwr->u.txpower.value = power / 4;
  return OK;
}

static int hosted_country(struct netdev_lowerhalf_s *dev,
                          struct iwreq *iwr, bool set)
{
  wifi_country_t country;
  char *code = iwr->u.data.pointer;
  esp_err_t ret;

  (void)dev;
  if (code == NULL)
    {
      return -EINVAL;
    }

  if (set)
    {
      if (iwr->u.data.length < 2)
        {
          return -EINVAL;
        }

      memset(&country, 0, sizeof(country));
      memcpy(country.cc, code, 2);
      country.schan = 1;
      country.nchan = strncmp(code, "US", 2) == 0 ||
                      strncmp(code, "CA", 2) == 0 ? 11 :
                      strncmp(code, "JP", 2) == 0 ? 14 : 13;
      country.policy = WIFI_COUNTRY_POLICY_MANUAL;
      return hosted_esp_to_errno(esp_wifi_remote_set_country(&country));
    }

  ret = esp_wifi_remote_get_country(&country);
  if (ret != ESP_OK)
    {
      return hosted_esp_to_errno(ret);
    }

  if (iwr->u.data.length < 2)
    {
      iwr->u.data.length = 2;
      return -E2BIG;
    }

  memcpy(code, country.cc, 2);
  if (iwr->u.data.length >= 3)
    {
      code[2] = '\0';
    }

  iwr->u.data.length = 2;
  return OK;
}

static int hosted_sensitivity(struct netdev_lowerhalf_s *dev,
                              struct iwreq *iwr, bool set)
{
  int rssi;
  esp_err_t ret;

  (void)dev;
  if (set)
    {
      return -EOPNOTSUPP;
    }

  ret = esp_wifi_remote_sta_get_rssi(&rssi);
  if (ret != ESP_OK)
    {
      return hosted_esp_to_errno(ret);
    }

  iwr->u.sens.value = -rssi;
  iwr->u.sens.fixed = 1;
  return OK;
}

/****************************************************************************
 * Name: hosted_scan
 *
 * Description:
 *   Scan for Wi-Fi networks.
 *
 ****************************************************************************/

static int hosted_scan(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set)
{
  struct esp_hosted_priv_s *priv = (struct esp_hosted_priv_s *)dev;
  wifi_ap_record_t *records = NULL;
  wifi_scan_config_t config;
  struct iw_scan_req *request;
  uint8_t target_bssid[HOSTED_MAC_LEN];
  const uint8_t broadcast_bssid[HOSTED_MAC_LEN] =
    {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint8_t target_ssid[IW_ESSID_MAX_SIZE + 1];
  uint16_t count;
  esp_err_t esp_ret;
  int ret;

  if (!priv->started || iwr == NULL)
    {
      return -ENETDOWN;
    }

  if (!set)
    {
      if (priv->scan_result == NULL)
        {
          iwr->u.data.length = 0;
          return -EAGAIN;
        }

      if (iwr->u.data.pointer == NULL ||
          iwr->u.data.length < priv->scan_result_size)
        {
          iwr->u.data.length = priv->scan_result_size;
          return -E2BIG;
        }

      memcpy(iwr->u.data.pointer, priv->scan_result,
             priv->scan_result_size);
      iwr->u.data.length = priv->scan_result_size;
      kmm_free(priv->scan_result);
      priv->scan_result = NULL;
      priv->scan_result_size = 0;
      return OK;
    }

  memset(&config, 0, sizeof(config));
  memset(target_bssid, 0, sizeof(target_bssid));
  memset(target_ssid, 0, sizeof(target_ssid));
  config.scan_type = WIFI_SCAN_TYPE_ACTIVE;

  if (iwr->u.data.pointer != NULL &&
      iwr->u.data.length >= sizeof(struct iw_scan_req))
    {
      request = iwr->u.data.pointer;
      config.scan_type = request->scan_type == IW_SCAN_TYPE_PASSIVE ?
                         WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE;

      if ((iwr->u.data.flags & IW_SCAN_THIS_ESSID) != 0 &&
          request->essid_len <= IW_ESSID_MAX_SIZE)
        {
          memcpy(target_ssid, request->essid, request->essid_len);
          config.ssid = target_ssid;
          config.show_hidden = true;
        }

      if ((iwr->u.data.flags & IW_SCAN_THIS_FREQ) != 0 &&
          request->num_channels == 1)
        {
          config.channel = hosted_freq_to_channel(&request->channel_list[0]);
          if (config.channel == 0)
            {
              return -EINVAL;
            }
        }

      if (request->bssid.sa_family == ARPHRD_ETHER &&
          memcmp(request->bssid.sa_data, broadcast_bssid,
                 HOSTED_MAC_LEN) != 0)
        {
          memcpy(target_bssid, request->bssid.sa_data, HOSTED_MAC_LEN);
          config.bssid = target_bssid;
        }
    }

  if (priv->scan_result != NULL)
    {
      kmm_free(priv->scan_result);
      priv->scan_result = NULL;
      priv->scan_result_size = 0;
    }

  esp_ret = esp_wifi_remote_scan_start(&config, true);
  if (esp_ret != ESP_OK)
    {
      return hosted_esp_to_errno(esp_ret);
    }

  esp_ret = esp_wifi_remote_scan_get_ap_num(&count);
  if (esp_ret != ESP_OK)
    {
      return hosted_esp_to_errno(esp_ret);
    }

  if (count > HOSTED_SCAN_MAX_APS)
    {
      count = HOSTED_SCAN_MAX_APS;
    }

  if (count > 0)
    {
      records = kmm_calloc(count, sizeof(*records));
      if (records == NULL)
        {
          return -ENOMEM;
        }

      esp_ret = esp_wifi_remote_scan_get_ap_records(&count, records);
      if (esp_ret != ESP_OK)
        {
          kmm_free(records);
          return hosted_esp_to_errno(esp_ret);
        }
    }

  ret = hosted_build_scan_results(priv, records, count);
  kmm_free(records);
  return ret;
}

static int hosted_range(struct netdev_lowerhalf_s *dev,
                        struct iwreq *iwr)
{
  struct iw_range *range;
  wifi_country_t country;
  esp_err_t ret;
  int channel;

  (void)dev;
  if (iwr == NULL || iwr->u.data.pointer == NULL)
    {
      return -EINVAL;
    }

  if (iwr->u.data.length < sizeof(*range))
    {
      iwr->u.data.length = sizeof(*range);
      return -E2BIG;
    }

  ret = esp_wifi_remote_get_country(&country);
  if (ret != ESP_OK)
    {
      return hosted_esp_to_errno(ret);
    }

  range = iwr->u.data.pointer;
  memset(range, 0, sizeof(*range));
  range->num_frequency = country.nchan;

  for (channel = 0;
       channel < country.nchan && channel < IW_MAX_FREQUENCIES;
       channel++)
    {
      int number = country.schan + channel;
      range->freq[channel].i = number;
      range->freq[channel].e = 0;
      range->freq[channel].m = number == 14 ? 2484 : 2407 + 5 * number;
    }

  range->num_frequency = channel;
  iwr->u.data.length = sizeof(*range);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_netdev_register
 *
 * Description:
 *   Register the ESP-Hosted network device.
 *
 ****************************************************************************/

int esp_hosted_netdev_register(void)
{
  int ret;

  /* Initialize spinlock */

  spin_lock_init(&g_hosted_sta.rx_lock);
  IOB_QINIT(&g_hosted_sta.netdev_rx_queue);
  g_hosted_sta.dev.quota[NETPKT_RX] = RX_BUF_COUNT;
  g_hosted_sta.dev.quota[NETPKT_TX] = TX_BUF_COUNT;
  g_hosted_sta.dev.rxtype = NETDEV_RX_THREAD;
  g_hosted_sta.dev.priority = CONFIG_ESP_HOSTED_TASK_PRIORITY;
  g_hosted_sta.wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  /* Register network device */

  ret = netdev_lower_register(&g_hosted_sta.dev, NET_LL_IEEE80211);
  if (ret < 0) {
    wlerr("ERROR: Failed to register network device: %d\n", ret);
    return ret;
  }

  wlinfo("ESP-Hosted network device registered\n");
  return OK;
}

/****************************************************************************
 * Name: esp_hosted_netdev_rx_notify
 *
 * Description:
 *   Notify the network device that new data is available.
 *   Called from the transport RX callback.
 *
 ****************************************************************************/

int esp_hosted_netdev_rx_notify(uint8_t *data, uint16_t len)
{
  struct esp_hosted_priv_s *priv = &g_hosted_sta;
  netpkt_t *pkt;
  irqstate_t flags;

  if (!priv->initialized || data == NULL || len == 0 ||
      len > CONFIG_NET_ETH_PKTSIZE) {
    return -ENODEV;
  }

  /* Allocate a packet */

  pkt = netpkt_alloc(&priv->dev, NETPKT_RX);
  if (!pkt) {
    wlerr("ERROR: Failed to allocate RX packet\n");
    return -ENOMEM;
  }

  /* Copy data into the packet */

  if (netpkt_copyin(&priv->dev, pkt, data, len, 0) < 0)
    {
      netpkt_free(&priv->dev, pkt, NETPKT_RX);
      return -ENOMEM;
    }

  netpkt_setdatalen(&priv->dev, pkt, len);

  /* Add to RX queue */

  flags = spin_lock_irqsave(&priv->rx_lock);
  if (netpkt_tryadd_queue(pkt, &priv->netdev_rx_queue) < 0)
    {
      spin_unlock_irqrestore(&priv->rx_lock, flags);
      netpkt_free(&priv->dev, pkt, NETPKT_RX);
      return -ENOBUFS;
    }

  spin_unlock_irqrestore(&priv->rx_lock, flags);

  /* Notify upper half */

  netdev_lower_rxready(&priv->dev);

  /* ESP-Hosted may deliver a complete SDIO stream in one burst.  The
   * lower-half RX worker has the same priority as the transport tasks on
   * this single-core target, so explicitly hand over after each frame.
   * Otherwise sdio_process_rx_task can consume the whole transport queue
   * before NuttX gets a chance to run ARP/IP input and exhaust RX quota.
   */

  sched_yield();

  return OK;
}

void esp_hosted_netdev_sta_connected(void)
{
  netdev_lower_carrier_on(&g_hosted_sta.dev);
}

void esp_hosted_netdev_sta_disconnected(void)
{
  netdev_lower_carrier_off(&g_hosted_sta.dev);
}
