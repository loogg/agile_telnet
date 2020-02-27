#ifndef __PKG_AGILE_TELNET_H
#define __PKG_AGILE_TELNET_H
#include <rtthread.h>
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

struct telnet_session
{
    struct rt_device device;
    uint8_t isconnected;
    rt_int32_t server_fd;
    rt_int32_t client_fd;
    int client_timeout;

    struct rt_ringbuffer rx_ringbuffer;
    rt_mutex_t rx_ringbuffer_lock;
    struct rt_ringbuffer tx_ringbuffer;
    rt_sem_t read_notice;
};

#ifdef __cplusplus
}
#endif

#endif
