/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted/esp_hosted_port.c
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
#include <unistd.h>
#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <pthread.h>
#include <mqueue.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <syslog.h>

#include <nuttx/board.h>
#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/wqueue.h>

#include "esp_hosted_port.h"
#include "esp_hosted_os_abstraction.h"
#include "esp_hosted.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TIMER_SIGNAL SIGRTMIN

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hosted_thread_arg {
    void (*start_routine)(void const *);
    void *arg;
};

struct hosted_timer_s {
    timer_t timer_id;
    void (*callback)(void *);
    void *arg;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

ESP_EVENT_DECLARE_BASE(ESP_HOSTED_EVENT);
ESP_EVENT_DECLARE_BASE(WIFI_EVENT);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hosted_thread_wrapper
 *
 * Description:
 *   Thread wrapper to adapt NuttX pthread to esp-hosted thread interface.
 *
 ****************************************************************************/

static void *hosted_thread_wrapper(void *arg)
{
    struct hosted_thread_arg *targ = (struct hosted_thread_arg *)arg;
    targ->start_routine(targ->arg);
    free(targ);
    return NULL;
}

/****************************************************************************
 * Name: hosted_timer_handler
 *
 * Description:
 *   Signal handler for POSIX timer expiration.
 *
 ****************************************************************************/

static void hosted_timer_handler(int signo, siginfo_t *info, void *context)
{
    struct hosted_timer_s *timer = (struct hosted_timer_s *)info->si_value.sival_ptr;
    if (timer && timer->callback) {
        timer->callback(timer->arg);
    }
}

/****************************************************************************
 * Memory Functions
 ****************************************************************************/

static void *hosted_memcpy(void *dest, const void *src, uint32_t size)
{
    if (size && (!dest || !src)) {
        return NULL;
    }
    return memcpy(dest, src, size);
}

static void *hosted_memset(void *buf, int val, size_t len)
{
    return memset(buf, val, len);
}

static void *hosted_malloc(size_t size)
{
    return kumm_malloc(size);
}

static void *hosted_calloc(size_t blk_no, size_t size)
{
    void *ptr = kumm_malloc(blk_no * size);
    if (ptr) {
        memset(ptr, 0, blk_no * size);
    }
    return ptr;
}

static void hosted_free(void *ptr)
{
    if (ptr) {
        kumm_free(ptr);
    }
}

static void *hosted_realloc(void *mem, size_t newsize)
{
    if (newsize == 0) {
        hosted_free(mem);
        return NULL;
    }
    return kumm_realloc(mem, newsize);
}

static void *hosted_malloc_align(size_t size, size_t align)
{
    return kumm_memalign(align, size);
}

static void hosted_free_align(void *ptr)
{
    hosted_free(ptr);
}

/****************************************************************************
 * Thread Functions
 ****************************************************************************/

static void *hosted_thread_create(const char *tname, uint32_t tprio,
                                  uint32_t tstack_size,
                                  void (*start_routine)(void const *),
                                  void *sr_arg)
{
    pthread_t *thread;
    pthread_attr_t attr;
    struct sched_param param;
    struct hosted_thread_arg *targ;
    int ret;

    thread = (pthread_t *)kumm_malloc(sizeof(pthread_t));
    if (!thread) {
        return NULL;
    }

    targ = (struct hosted_thread_arg *)kumm_malloc(sizeof(struct hosted_thread_arg));
    if (!targ) {
        kumm_free(thread);
        return NULL;
    }

    targ->start_routine = start_routine;
    targ->arg = sr_arg;

    pthread_attr_init(&attr);
    param.sched_priority = tprio;
    pthread_attr_setschedparam(&attr, &param);
    pthread_attr_setstacksize(&attr, tstack_size);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(thread, &attr, hosted_thread_wrapper, targ);
    if (ret != 0) {
        kumm_free(thread);
        kumm_free(targ);
        return NULL;
    }

    return thread;
}

static int hosted_thread_cancel(void *thread_handle)
{
    if (thread_handle) {
        pthread_t *thread = (pthread_t *)thread_handle;
        pthread_cancel(*thread);
        kumm_free(thread);
    }
    return 0;
}

static void hosted_thread_yield(void)
{
    sched_yield();
}

/****************************************************************************
 * Sleep Functions
 ****************************************************************************/

static unsigned int hosted_msleep(unsigned int mseconds)
{
    nxsig_usleep(mseconds * 1000);
    return 0;
}

static unsigned int hosted_usleep(unsigned int useconds)
{
    nxsig_usleep(useconds);
    return 0;
}

static unsigned int hosted_sleep(unsigned int seconds)
{
    nxsig_sleep(seconds);
    return 0;
}

static unsigned int hosted_blocking_delay(unsigned int number)
{
    /* Simple busy-wait delay for short delays */

    volatile unsigned int i;
    for (i = 0; i < number * 1000; i++) {
    }
    return 0;
}

/****************************************************************************
 * Queue Functions
 ****************************************************************************/

static void *hosted_create_queue(uint32_t qnum_elem, uint32_t qitem_size)
{
    mqd_t *mqd;
    struct mq_attr attr;
    char mqname[32];
    static int queue_id = 0;

    mqd = (mqd_t *)kumm_malloc(sizeof(mqd_t));
    if (!mqd) {
        return NULL;
    }

    snprintf(mqname, sizeof(mqname), "/hosted_q%d", queue_id++);

    attr.mq_flags = 0;
    attr.mq_maxmsg = qnum_elem;
    attr.mq_msgsize = qitem_size;
    attr.mq_curmsgs = 0;

    *mqd = mq_open(mqname, O_CREAT | O_RDWR, 0666, &attr);
    if (*mqd == (mqd_t)-1) {
        kumm_free(mqd);
        return NULL;
    }

    return mqd;
}

static int hosted_queue_item(void *queue_handle, void *item, int timeout)
{
    mqd_t *mqd = (mqd_t *)queue_handle;
    struct mq_attr attr;
    unsigned int prio = 0;

    if (!mqd || !item) {
        return -EINVAL;
    }

    mq_getattr(*mqd, &attr);

    if (timeout == HOSTED_BLOCK_MAX) {
        return mq_send(*mqd, (const char *)item, attr.mq_msgsize, prio);
    } else if (timeout == 0) {
        /* Non-blocking */

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 1000000; /* 1ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        return mq_timedsend(*mqd, (const char *)item, attr.mq_msgsize, prio, &ts);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout / 1000;
        ts.tv_nsec += (timeout % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        return mq_timedsend(*mqd, (const char *)item, attr.mq_msgsize, prio, &ts);
    }
}

static int hosted_dequeue_item(void *queue_handle, void *item, int timeout)
{
    mqd_t *mqd = (mqd_t *)queue_handle;
    struct mq_attr attr;
    unsigned int prio = 0;
    ssize_t ret;

    if (!mqd || !item) {
        return -EINVAL;
    }

    mq_getattr(*mqd, &attr);

    if (timeout == HOSTED_BLOCK_MAX) {
        ret = mq_receive(*mqd, (char *)item, attr.mq_msgsize, &prio);
    } else if (timeout == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 1000000; /* 1ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        ret = mq_timedreceive(*mqd, (char *)item, attr.mq_msgsize, &prio, &ts);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout / 1000;
        ts.tv_nsec += (timeout % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        ret = mq_timedreceive(*mqd, (char *)item, attr.mq_msgsize, &prio, &ts);
    }

    return (ret < 0) ? -errno : 0;
}

static int hosted_queue_msg_waiting(void *queue_handle)
{
    mqd_t *mqd = (mqd_t *)queue_handle;
    struct mq_attr attr;

    if (!mqd) {
        return 0;
    }

    mq_getattr(*mqd, &attr);
    return attr.mq_curmsgs;
}

static int hosted_destroy_queue(void *queue_handle)
{
    mqd_t *mqd = (mqd_t *)queue_handle;

    if (mqd) {
        mq_close(*mqd);
        kumm_free(mqd);
    }
    return 0;
}

static int hosted_reset_queue(void *queue_handle)
{
    /* NuttX doesn't have a direct queue reset, drain messages */

    mqd_t *mqd = (mqd_t *)queue_handle;
    struct mq_attr attr;
    char buf[256];
    unsigned int prio;

    if (!mqd) {
        return -EINVAL;
    }

    mq_getattr(*mqd, &attr);
    while (attr.mq_curmsgs > 0) {
        mq_timedreceive(*mqd, buf, attr.mq_msgsize, &prio, NULL);
        attr.mq_curmsgs--;
    }

    return 0;
}

/****************************************************************************
 * Mutex Functions
 ****************************************************************************/

static void *hosted_create_mutex(void)
{
    mutex_t *mutex = (mutex_t *)kumm_malloc(sizeof(mutex_t));
    if (mutex) {
        nxmutex_init(mutex);
    }
    return mutex;
}

static int hosted_lock_mutex(void *mutex_handle, int timeout_ms)
{
    mutex_t *mutex = (mutex_t *)mutex_handle;
    int ret;

    if (!mutex) {
        return -EINVAL;
    }

    if (timeout_ms == HOSTED_BLOCK_MAX) {
        ret = nxmutex_lock(mutex);
    } else if (timeout_ms == 0) {
        ret = nxmutex_trylock(mutex);
    } else {
        /* NuttX mutex doesn't support timed lock directly,
         * use trylock with retry
         */

        int elapsed = 0;
        while (elapsed < timeout_ms) {
            ret = nxmutex_trylock(mutex);
            if (ret == 0) {
                return 0;
            }
            nxsig_usleep(1000); /* 1ms */
            elapsed++;
        }
        ret = -ETIMEDOUT;
    }

    return ret;
}

static int hosted_unlock_mutex(void *mutex_handle)
{
    mutex_t *mutex = (mutex_t *)mutex_handle;
    if (!mutex) {
        return -EINVAL;
    }
    return nxmutex_unlock(mutex);
}

static int hosted_destroy_mutex(void *mutex_handle)
{
    mutex_t *mutex = (mutex_t *)mutex_handle;
    if (mutex) {
        nxmutex_destroy(mutex);
        kumm_free(mutex);
    }
    return 0;
}

/****************************************************************************
 * Semaphore Functions
 ****************************************************************************/

static void *hosted_create_semaphore(int maxCount)
{
    sem_t *sem = (sem_t *)kumm_malloc(sizeof(sem_t));
    if (sem) {
        nxsem_init(sem, 0, maxCount);
    }
    return sem;
}

static int hosted_post_semaphore(void *semaphore_handle)
{
    sem_t *sem = (sem_t *)semaphore_handle;
    if (!sem) {
        return -EINVAL;
    }
    return nxsem_post(sem);
}

static int hosted_post_semaphore_from_isr(void *semaphore_handle)
{
    sem_t *sem = (sem_t *)semaphore_handle;
    if (!sem) {
        return -EINVAL;
    }
    return nxsem_post(sem);
}

static int hosted_get_semaphore(void *semaphore_handle, int timeout_ms)
{
    sem_t *sem = (sem_t *)semaphore_handle;
    int ret;

    if (!sem) {
        return -EINVAL;
    }

    if (timeout_ms == HOSTED_BLOCK_MAX) {
        ret = nxsem_wait(sem);
    } else if (timeout_ms == 0) {
        ret = nxsem_trywait(sem);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        ret = nxsem_timedwait(sem, &ts);
    }

    return ret;
}

static int hosted_destroy_semaphore(void *semaphore_handle)
{
    sem_t *sem = (sem_t *)semaphore_handle;
    if (sem) {
        nxsem_destroy(sem);
        kumm_free(sem);
    }
    return 0;
}

/****************************************************************************
 * Timer Functions
 ****************************************************************************/

static void *hosted_timer_start(const char *name, int duration_ms,
                                int type, void (*timeout_handler)(void *),
                                void *arg)
{
    struct hosted_timer_s *timer;
    struct sigevent sev;
    struct itimerspec its;

    timer = (struct hosted_timer_s *)kumm_malloc(sizeof(struct hosted_timer_s));
    if (!timer) {
        return NULL;
    }

    timer->callback = timeout_handler;
    timer->arg = arg;

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = TIMER_SIGNAL;
    sev.sigev_value.sival_ptr = timer;

    if (timer_create(CLOCK_REALTIME, &sev, &timer->timer_id) != 0) {
        kumm_free(timer);
        return NULL;
    }

    its.it_value.tv_sec = duration_ms / 1000;
    its.it_value.tv_nsec = (duration_ms % 1000) * 1000000;
    its.it_interval.tv_sec = (type == 1) ? its.it_value.tv_sec : 0;
    its.it_interval.tv_nsec = (type == 1) ? its.it_value.tv_nsec : 0;

    if (timer_settime(timer->timer_id, 0, &its, NULL) != 0) {
        timer_delete(timer->timer_id);
        kumm_free(timer);
        return NULL;
    }

    return timer;
}

static int hosted_timer_stop(void *timer_handle)
{
    struct hosted_timer_s *timer = (struct hosted_timer_s *)timer_handle;
    if (timer) {
        timer_delete(timer->timer_id);
        kumm_free(timer);
    }
    return 0;
}

static uint64_t hosted_get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/****************************************************************************
 * GPIO Functions
 ****************************************************************************/

static int hosted_config_gpio(void *gpio_port, uint32_t gpio_num, uint32_t mode)
{
    /* TODO: Implement using NuttX GPIO API */

    return 0;
}

static int hosted_config_gpio_as_interrupt(void *gpio_port, uint32_t gpio_num,
                                           uint32_t intr_type,
                                           void (*gpio_isr_handler)(void *arg),
                                           void *arg)
{
    /* TODO: Implement using NuttX GPIO IRQ API */

    return 0;
}

static int hosted_teardown_gpio_interrupt(void *gpio_port, uint32_t gpio_num)
{
    /* TODO: Implement using NuttX GPIO IRQ API */

    return 0;
}

static int hosted_read_gpio(void *gpio_port, uint32_t gpio_num)
{
    /* TODO: Implement using NuttX GPIO API */

    return 0;
}

static int hosted_write_gpio(void *gpio_port, uint32_t gpio_num, uint32_t value)
{
    /* TODO: Implement using NuttX GPIO API */

    return 0;
}

static int hosted_pull_gpio(void *gpio_port, uint32_t gpio_num,
                            uint32_t pull_value, uint32_t enable)
{
    /* TODO: Implement using NuttX GPIO API */

    return 0;
}

static int hosted_hold_gpio(void *gpio_port, uint32_t gpio_num,
                            uint32_t hold_value)
{
    /* TODO: Implement using NuttX GPIO API */

    return 0;
}

static int hosted_get_host_wakeup_or_reboot_reason(void)
{
    return 0;
}

/****************************************************************************
 * Transport Functions (Stubs - implemented by transport adapters)
 ****************************************************************************/

static void *hosted_bus_init(void)
{
    /* Implemented by transport adapter */

    return NULL;
}

static int hosted_bus_deinit(void *ctx)
{
    /* Implemented by transport adapter */

    return 0;
}

static int hosted_do_bus_transfer(void *transfer_context)
{
    /* Implemented by transport adapter */

    return -ENOSYS;
}

/****************************************************************************
 * Event Functions
 ****************************************************************************/

static int hosted_event_wifi_post(int32_t event_id, void *event_data,
                                  size_t event_data_size,
                                  uint32_t ticks_to_wait)
{
    /* TODO: Dispatch via NuttX work queue */

    wlinfo("Wi-Fi event: %ld\n", (long)event_id);
    return 0;
}

static int hosted_event_post(esp_event_base_t event_base, int32_t event_id,
                             void *event_data, size_t event_data_size,
                             uint32_t ticks_to_wait)
{
    /* TODO: Dispatch via NuttX work queue */

    wlinfo("Event: %s:%ld\n", event_base, (long)event_id);
    return 0;
}

/****************************************************************************
 * Misc Functions
 ****************************************************************************/

static void hosted_printf(int level, const char *tag, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog(level, format, ap);
    va_end(ap);
}

static void hosted_init_hook(void)
{
    /* Called after esp_hosted_init completes */
}

static int hosted_restart_host(void)
{
    board_reset(0);
    return 0;
}

static int hosted_config_host_power_save_hal_impl(uint32_t power_save_type,
                                                   void *gpio_port,
                                                   uint32_t gpio_num,
                                                   int level)
{
    /* Not implemented for NuttX */

    return 0;
}

static int hosted_start_host_power_save_hal_impl(uint32_t power_save_type)
{
    /* Not implemented for NuttX */

    return 0;
}

/****************************************************************************
 * Mempool Lock Functions
 ****************************************************************************/

#ifdef H_USE_MEMPOOL
static void *hosted_create_lock_mempool(void)
{
    return hosted_create_mutex();
}

static void hosted_lock_mempool(void *lock_handle)
{
    hosted_lock_mutex(lock_handle, HOSTED_BLOCK_MAX);
}

static void hosted_unlock_mempool(void *lock_handle)
{
    hosted_unlock_mutex(lock_handle);
}

static void hosted_destroy_lock_mempool(void *lock_handle)
{
    hosted_destroy_mutex(lock_handle);
}
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Global OS interface functions structure */

hosted_osi_funcs_t g_hosted_osi_funcs = {
    /* Memory */
    ._h_memcpy = hosted_memcpy,
    ._h_memset = hosted_memset,
    ._h_malloc = hosted_malloc,
    ._h_calloc = hosted_calloc,
    ._h_free = hosted_free,
    ._h_realloc = hosted_realloc,
    ._h_malloc_align = hosted_malloc_align,
    ._h_free_align = hosted_free_align,

    /* Thread */
    ._h_thread_create = hosted_thread_create,
    ._h_thread_cancel = hosted_thread_cancel,
    ._h_thread_yield = hosted_thread_yield,

    /* Sleep */
    ._h_msleep = hosted_msleep,
    ._h_usleep = hosted_usleep,
    ._h_sleep = hosted_sleep,
    ._h_blocking_delay = hosted_blocking_delay,

    /* Queue */
    ._h_queue_item = hosted_queue_item,
    ._h_create_queue = hosted_create_queue,
    ._h_dequeue_item = hosted_dequeue_item,
    ._h_queue_msg_waiting = hosted_queue_msg_waiting,
    ._h_destroy_queue = hosted_destroy_queue,
    ._h_reset_queue = hosted_reset_queue,

    /* Mutex */
    ._h_unlock_mutex = hosted_unlock_mutex,
    ._h_create_mutex = hosted_create_mutex,
    ._h_lock_mutex = hosted_lock_mutex,
    ._h_destroy_mutex = hosted_destroy_mutex,

    /* Semaphore */
    ._h_post_semaphore = hosted_post_semaphore,
    ._h_post_semaphore_from_isr = hosted_post_semaphore_from_isr,
    ._h_create_semaphore = hosted_create_semaphore,
    ._h_get_semaphore = hosted_get_semaphore,
    ._h_destroy_semaphore = hosted_destroy_semaphore,

    /* Timer */
    ._h_timer_stop = hosted_timer_stop,
    ._h_timer_start = hosted_timer_start,
    ._h_get_time_ms = hosted_get_time_ms,

#ifdef H_USE_MEMPOOL
    /* Mempool lock */
    ._h_create_lock_mempool = hosted_create_lock_mempool,
    ._h_lock_mempool = hosted_lock_mempool,
    ._h_unlock_mempool = hosted_unlock_mempool,
    ._h_destroy_lock_mempool = hosted_destroy_lock_mempool,
#endif

    /* GPIO */
    ._h_config_gpio = hosted_config_gpio,
    ._h_config_gpio_as_interrupt = hosted_config_gpio_as_interrupt,
    ._h_teardown_gpio_interrupt = hosted_teardown_gpio_interrupt,
    ._h_read_gpio = hosted_read_gpio,
    ._h_write_gpio = hosted_write_gpio,
    ._h_pull_gpio = hosted_pull_gpio,
    ._h_hold_gpio = hosted_hold_gpio,
    ._h_get_host_wakeup_or_reboot_reason = hosted_get_host_wakeup_or_reboot_reason,

    /* Bus */
    ._h_bus_init = hosted_bus_init,
    ._h_bus_deinit = hosted_bus_deinit,
    ._h_do_bus_transfer = hosted_do_bus_transfer,

    /* Event */
    ._h_event_wifi_post = hosted_event_wifi_post,
    ._h_printf = hosted_printf,
    ._h_hosted_init_hook = hosted_init_hook,

    /* SDIO */
    ._h_sdio_card_init = NULL,
    ._h_sdio_card_deinit = NULL,
    ._h_sdio_read_reg = NULL,
    ._h_sdio_write_reg = NULL,
    ._h_sdio_read_block = NULL,
    ._h_sdio_write_block = NULL,
    ._h_sdio_wait_slave_intr = NULL,

    /* SPI HD */
    ._h_spi_hd_read_reg = NULL,
    ._h_spi_hd_write_reg = NULL,
    ._h_spi_hd_read_dma = NULL,
    ._h_spi_hd_write_dma = NULL,
    ._h_spi_hd_set_data_lines = NULL,
    ._h_spi_hd_send_cmd9 = NULL,

    /* UART */
    ._h_uart_read = NULL,
    ._h_uart_write = NULL,
    ._h_uart_flush_input = NULL,

    /* Misc */
    ._h_restart_host = hosted_restart_host,
    ._h_config_host_power_save_hal_impl = hosted_config_host_power_save_hal_impl,
    ._h_start_host_power_save_hal_impl = hosted_start_host_power_save_hal_impl,
    ._h_event_post = hosted_event_post,
};

/* Global hosted configuration */

struct hosted_config_t g_h = HOSTED_CONFIG_INIT_DEFAULT();

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_port_init
 *
 * Description:
 *   Initialize the ESP-Hosted port layer.
 *
 ****************************************************************************/

int esp_hosted_port_init(void)
{
    /* Install timer signal handler */

    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = hosted_timer_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(TIMER_SIGNAL, &sa, NULL);

    return 0;
}
