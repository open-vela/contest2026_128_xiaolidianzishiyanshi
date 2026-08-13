/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/port_esp_hosted_host_os.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NuttX port OS abstraction layer for ESP-Hosted.
 *
 ****************************************************************************/

#ifndef __PORT_ESP_HOSTED_HOST_OS_H
#define __PORT_ESP_HOSTED_HOST_OS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_hosted_os_abstraction.h"
#include "port_esp_hosted_host_config.h"
#include "esp_log.h"
#include "esp_event.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MCU_SYS  1

/* Task handle types - use void* for NuttX compatibility */

#define thread_handle_t    void *
#define queue_handle_t     void *
#define semaphore_handle_t void *
#define mutex_handle_t     void *

#define spinlock_handle_t  void *
#define gpio_port_handle_t (void *)

#define FAST_RAM_ATTR

#define H_GPIO_PORT_DEFAULT  -1

typedef int gpio_pin_state_t;

#define HOSTED_BLOCK_MAX  -1

/* Task configuration */

#define RPC_TASK_STACK_SIZE   CONFIG_ESP_HOSTED_TASK_STACK_SIZE
#define RPC_TASK_PRIO         CONFIG_ESP_HOSTED_TASK_PRIORITY
#define DFLT_TASK_STACK_SIZE  CONFIG_ESP_HOSTED_TASK_STACK_SIZE
#define DFLT_TASK_PRIO        CONFIG_ESP_HOSTED_TASK_PRIORITY

/* GPIO mode definitions */

#define H_GPIO_MODE_DEF_DISABLE   0
#define H_GPIO_MODE_DEF_INPUT     (1 << 0)
#define H_GPIO_MODE_DEF_OUTPUT    (1 << 1)
#define H_GPIO_MODE_DEF_OD        (1 << 2)

enum
{
  H_GPIO_MODE_DISABLE = H_GPIO_MODE_DEF_DISABLE,
  H_GPIO_MODE_INPUT = H_GPIO_MODE_DEF_INPUT,
  H_GPIO_MODE_OUTPUT = H_GPIO_MODE_DEF_OUTPUT,
  H_GPIO_MODE_OUTPUT_OD = H_GPIO_MODE_DEF_OUTPUT | H_GPIO_MODE_DEF_OD,
  H_GPIO_MODE_INPUT_OUTPUT_OD = H_GPIO_MODE_DEF_INPUT |
                                H_GPIO_MODE_DEF_OUTPUT |
                                H_GPIO_MODE_DEF_OD,
  H_GPIO_MODE_INPUT_OUTPUT = H_GPIO_MODE_DEF_INPUT |
                             H_GPIO_MODE_DEF_OUTPUT,
};

#define H_GPIO_PULL_UP    1
#define H_GPIO_PULL_DOWN  0

/* Return codes */

#define RET_OK          0
#define RET_FAIL       -1
#define RET_INVALID    -2
#define RET_FAIL_MEM   -3
#define RET_FAIL4      -4
#define RET_FAIL_TIMEOUT -5

/* Memory alignment */

#define HOSTED_MEM_ALIGNMENT_4   4
#define HOSTED_MEM_ALIGNMENT_32  32
#define HOSTED_MEM_ALIGNMENT_64  64

/* Hardware type enumeration */

enum hardware_type_e
{
  HARDWARE_TYPE_ESP32,
  HARDWARE_TYPE_OTHER_ESP_CHIPSETS,
  HARDWARE_TYPE_INVALID,
};

/* Time conversion macros */

#define MILLISEC_TO_SEC(x)        ((x) / 1000)
#define TICKS_PER_SEC(x)          (1000 * (x) / CONFIG_USEC_PER_TICK)
#define SEC_TO_MILLISEC(x)        (1000 * (x))
#define SEC_TO_MICROSEC(x)        (1000 * 1000 * (x))
#define MILLISEC_TO_MICROSEC(x)   (1000 * (x))

/* Memory dump macro - no-op on NuttX */

#define MEM_DUMP(s) do { } while (0)

/* Handle management macros */

#define HOSTED_CREATE_HANDLE(tYPE, hANDLE) { \
    hANDLE = (tYPE *)g_h.funcs->_h_malloc(sizeof(tYPE)); \
    if (!hANDLE) { \
        ESP_LOGE(TAG, "%s:%u Mem alloc fail while create handle", \
                 __func__, __LINE__); \
        return NULL; \
    } \
}

#define HOSTED_FREE_HANDLE(handle) { \
    if (handle) { \
        g_h.funcs->_h_free(handle); \
        handle = NULL; \
    } \
}

#define HOSTED_FREE(buff) if (buff) { \
    g_h.funcs->_h_free(buff); \
    buff = NULL; \
}

#define HOSTED_CALLOC(struct_name, buff, nbytes, gotosym) do { \
    buff = (struct_name *)g_h.funcs->_h_calloc(1, nbytes); \
    if (!buff) { \
        ESP_LOGE(TAG, "%s, Failed to allocate memory", __func__); \
        goto gotosym; \
    } \
} while (0);

#define HOSTED_MALLOC(struct_name, buff, nbytes, gotosym) do { \
    buff = (struct_name *)g_h.funcs->_h_malloc(nbytes); \
    if (!buff) { \
        ESP_LOGE(TAG, "%s, Failed to allocate memory", __func__); \
        goto gotosym; \
    } \
} while (0);

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Driver handle - forward declaration */

struct serial_drv_handle_t;

/* Timer handle */

struct timer_handle_t;

/****************************************************************************
 * Timer Types
 ****************************************************************************/

enum
{
  H_TIMER_TYPE_ONESHOT = 0,
  H_TIMER_TYPE_PERIODIC
};

/****************************************************************************
 * Blocking Constants
 ****************************************************************************/

#define HOSTED_BLOCKING  -1
#define HOSTED_NO_WAIT    0

/****************************************************************************
 * Payload Size
 ****************************************************************************/

#ifndef MAX_PAYLOAD_SIZE
#  define MAX_PAYLOAD_SIZE (MAX_TRANSPORT_BUFFER_SIZE - H_ESP_PAYLOAD_HEADER_OFFSET)
#endif

/****************************************************************************
 * Custom Message Handlers
 ****************************************************************************/

#ifndef H_MAX_CUSTOM_MSG_HANDLERS
#  define H_MAX_CUSTOM_MSG_HANDLERS  10
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern struct mempool *nw_mp_g;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Hosted event declarations */

ESP_EVENT_DECLARE_BASE(WIFI_EVENT);

#endif /* __PORT_ESP_HOSTED_HOST_OS_H */
