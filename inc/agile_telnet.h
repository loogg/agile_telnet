#ifndef __PKG_AGILE_TELNET_H
#define __PKG_AGILE_TELNET_H
#include <rtthread.h>
#include <rtdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

struct agile_telnet
{
    uint8_t isconnected;
    int server_fd;
    int client_fd;
    int client_timeout;
    rt_device_t tlnt_dev;
    int tx_fd;

    struct rt_ringbuffer *rx_rb;
    struct rt_ringbuffer *tx_rb;
};

#ifdef __cplusplus
}
#endif

#endif
