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

#include <arpa/inet.h>
#include <nuttx/wireless/wireless.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>

#include "esp_hosted_port.h"
#include "esp_hosted_netdev.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RX_BUF_COUNT    16
#define TX_BUF_COUNT    1
#define CONNECT_TIMEOUT 30

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_hosted_priv_s
{
  /* Upper-half interface */

  struct netdev_lowerhalf_s dev;

  /* Lower-half data */

  bool initialized;
  uint32_t mode;

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
static int hosted_passwd(struct netdev_lowerhalf_s *dev,
                         struct iwreq *iwr, bool set);
static int hosted_mode(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set);
static int hosted_auth(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set);
static int hosted_freq(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set);
static int hosted_scan(struct netdev_lowerhalf_s *dev,
                       struct iwreq *iwr, bool set);

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
    .ioctl = hosted_ioctl,
    .reclaim = hosted_reclaim,
};

/* Wireless operations */

static const struct wireless_ops_s g_wireless_ops =
{
  .connect = hosted_connect,
  .disconnect = hosted_disconnect,
  .essid = hosted_essid,
  .passwd = hosted_passwd,
  .mode = hosted_mode,
  .auth = hosted_auth,
  .freq = hosted_freq,
  .scan = hosted_scan,
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

  /* Initialize ESP-Hosted if not done */

  if (!priv->initialized) {
    ret = esp_hosted_init();
    if (ret < 0) {
      wlerr("ERROR: Failed to initialize ESP-Hosted: %d\n", ret);
      return ret;
    }

    ret = esp_hosted_connect_to_slave();
    if (ret < 0) {
      wlerr("ERROR: Failed to connect to slave: %d\n", ret);
      return ret;
    }

    priv->initialized = true;
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

  /* Copy data from the packet to the flat buffer */

  netpkt_copyout(dev, priv->flatbuf, pkt, len, 0);

  /* Send via ESP-Hosted */

  ret = esp_hosted_tx(priv->flatbuf, len);
  if (ret < 0) {
    wlerr("ERROR: Failed to transmit packet: %d\n", ret);
    return ret;
  }

  /* Free the packet after sending */

  netpkt_free(dev, pkt, NETPKT_TX);

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
  /* TODO: Implement ioctl handler */

  return -ENOSYS;
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
  /* TODO: Implement using ESP-Hosted RPC */

  wlinfo("Wi-Fi connect requested\n");
  return -ENOSYS;
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
  /* TODO: Implement using ESP-Hosted RPC */

  wlinfo("Wi-Fi disconnect requested\n");
  return -ENOSYS;
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
  /* TODO: Implement using ESP-Hosted RPC */

  wlinfo("ESSID %s requested\n", set ? "set" : "get");
  return -ENOSYS;
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
  /* TODO: Implement using ESP-Hosted RPC */

  wlinfo("Password %s requested\n", set ? "set" : "get");
  return -ENOSYS;
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
  /* TODO: Implement using ESP-Hosted RPC */

  wlinfo("Mode %s requested\n", set ? "set" : "get");
  return -ENOSYS;
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
  /* TODO: Implement using ESP-Hosted RPC */

  wlinfo("Auth %s requested\n", set ? "set" : "get");
  return -ENOSYS;
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
  /* TODO: Implement using ESP-Hosted RPC */

  wlinfo("Freq %s requested\n", set ? "set" : "get");
  return -ENOSYS;
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
  /* TODO: Implement using ESP-Hosted RPC */

  wlinfo("Scan requested\n");
  return -ENOSYS;
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

  if (!priv->initialized) {
    return -ENODEV;
  }

  /* Allocate a packet */

  pkt = netpkt_alloc(&priv->dev, NETPKT_RX);
  if (!pkt) {
    wlerr("ERROR: Failed to allocate RX packet\n");
    return -ENOMEM;
  }

  /* Copy data into the packet */

  netpkt_copyin(&priv->dev, pkt, data, len, 0);
  netpkt_setdatalen(&priv->dev, pkt, len);

  /* Add to RX queue */

  flags = spin_lock_irqsave(&priv->rx_lock);
  netpkt_add_queue(&priv->netdev_rx_queue, pkt);
  spin_unlock_irqrestore(&priv->rx_lock, flags);

  /* Notify upper half */

  netdev_lower_rxready(&priv->dev);

  return OK;
}
