/*************************************************
 All rights reserved.
 File name:     agile_telnet.c
 Description:   agile_telnet源码
 History:
 1. Version:      v1.0.0
    Date:         2020-02-27
    Author:       Longwei Ma
    Modification: 新建版本
*************************************************/

#include <rthw.h>

#ifdef PKG_USING_AGILE_TELNET

#include <agile_telnet.h>
#include <dfs_posix.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <msh.h>
#include <shell.h>

/* 线程堆栈大小 */
#ifndef PKG_AGILE_TELNET_THREAD_STACK_SIZE
#define PKG_AGILE_TELNET_THREAD_STACK_SIZE          2048
#endif

/* 线程优先级 */
#ifndef PKG_AGILE_TELNET_THREAD_PRIORITY
#define PKG_AGILE_TELNET_THREAD_PRIORITY            RT_THREAD_PRIORITY_MAX - 6
#endif

/* 监听端口 */
#ifndef PKG_AGILE_TELNET_PORT
#define PKG_AGILE_TELNET_PORT                       23
#endif

/* 接收缓冲区大小 */
#ifndef PKG_AGILE_TELNET_RX_BUFFER_SIZE
#define PKG_AGILE_TELNET_RX_BUFFER_SIZE             256
#endif

/* 发送缓冲区大小 */
#ifndef PKG_AGILE_TELNET_TX_BUFFER_SIZE
#define PKG_AGILE_TELNET_TX_BUFFER_SIZE             2048
#endif

/* 客户端空闲超时时间(单位：min) */
#ifndef PKG_AGILE_TELNET_CLIENT_DEFAULT_TIMEOUT
#define PKG_AGILE_TELNET_CLIENT_DEFAULT_TIMEOUT     3
#endif

static struct telnet_session telnet = {0};

/* RT-Thread Device Driver Interface */
static rt_err_t telnet_init(rt_device_t dev)
{
    return RT_EOK;
}

static rt_err_t telnet_open(rt_device_t dev, rt_uint16_t oflag)
{
    return RT_EOK;
}

static rt_err_t telnet_close(rt_device_t dev)
{
    return RT_EOK;
}

static rt_size_t telnet_read(rt_device_t dev, rt_off_t pos, void* buffer, rt_size_t size)
{
    rt_size_t result = 0;

    do
    {
        rt_mutex_take(telnet.rx_ringbuffer_lock, RT_WAITING_FOREVER);
        result = rt_ringbuffer_get(&(telnet.rx_ringbuffer), buffer, size);
        rt_mutex_release(telnet.rx_ringbuffer_lock);

        if(result == 0)
        {
            rt_sem_take(telnet.read_notice, RT_WAITING_FOREVER);
        }
    } while (result == 0);

    return result;
}

static rt_size_t telnet_write(rt_device_t dev, rt_off_t pos, const void* buffer, rt_size_t size)
{
    if(telnet.isconnected == 0)
        return size;
    
    if(size > 0)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        rt_ringbuffer_put(&telnet.tx_ringbuffer, buffer, size);
        rt_hw_interrupt_enable(level);
    }

    return size;
}

static rt_err_t telnet_control(rt_device_t dev, int cmd, void *args)
{
    return RT_EOK;
}

static int process_tx(struct telnet_session* telnet)
{
    rt_size_t length = 0;
    rt_uint8_t tx_buffer[100];
    rt_size_t tx_len = 0;

    while(1)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        length = rt_ringbuffer_get(&(telnet->tx_ringbuffer), tx_buffer, sizeof(tx_buffer));
        rt_hw_interrupt_enable(level);

        if(length > 0)
        {
            tx_len += length;
            send(telnet->client_fd, tx_buffer, length, 0);
        }
        else
        {
            break;
        }
    }

    return tx_len;
}

static void process_rx(struct telnet_session *telnet, rt_uint8_t *data, rt_size_t length)
{
    rt_mutex_take(telnet->rx_ringbuffer_lock, RT_WAITING_FOREVER);
    while(length)
    {
        if(*data != '\r') /* ignore '\r' */
        {
            rt_ringbuffer_putchar(&(telnet->rx_ringbuffer), *data);
        }

        data++;
        length--;
    }
    rt_mutex_release(telnet->rx_ringbuffer_lock);
}

/* telnet server thread entry */
static void telnet_thread(void* parameter)
{
#define RECV_BUF_LEN 64

    struct sockaddr_in addr;
    socklen_t addr_size;
    rt_uint8_t recv_buf[RECV_BUF_LEN];
    int max_fd = -1;
    fd_set readset, writeset, exceptset;
    rt_tick_t client_tick_timeout = rt_tick_get();
    int rc;
    // select超时时间
    struct timeval select_timeout;
    select_timeout.tv_sec = 10;
    select_timeout.tv_usec = 0;

    telnet.server_fd = -1;
    telnet.client_fd = -1;
    telnet.isconnected = 0;
    telnet.client_timeout = PKG_AGILE_TELNET_CLIENT_DEFAULT_TIMEOUT;

    rt_thread_mdelay(5000);
_telnet_start:
    if ((telnet.server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        goto _telnet_restart;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(PKG_AGILE_TELNET_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    rt_memset(&(addr.sin_zero), 0, sizeof(addr.sin_zero));
    if (bind(telnet.server_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr)) < 0)
        goto _telnet_restart;

    if (listen(telnet.server_fd, 1) < 0)
        goto _telnet_restart;

    while (1)
    {
        FD_ZERO(&readset);
        FD_ZERO(&writeset);
        FD_ZERO(&exceptset);

        FD_SET(telnet.server_fd, &readset);
        FD_SET(telnet.server_fd, &exceptset);

        max_fd = telnet.server_fd;
        if(telnet.client_fd >= 0)
        {
            FD_SET(telnet.client_fd, &readset);
            FD_SET(telnet.client_fd, &writeset);
            FD_SET(telnet.client_fd, &exceptset);
            if(max_fd < telnet.client_fd)
                max_fd = telnet.client_fd;
        }

        rc = select(max_fd + 1, &readset, &writeset, &exceptset, &select_timeout);
        if(rc < 0)
            goto _telnet_restart;
        else if(rc > 0)
        {
            //服务器事件
            if(FD_ISSET(telnet.server_fd, &exceptset))
                goto _telnet_restart;

            if(FD_ISSET(telnet.server_fd, &readset))
            {
                int client_sock_fd = accept(telnet.server_fd, (struct sockaddr *)&addr, &addr_size);
                if(client_sock_fd < 0)
                    goto _telnet_restart;
                else
                {
                    if(telnet.client_fd >= 0)
                    {
                        telnet.isconnected = 0;
                        close(telnet.client_fd);
                        telnet.client_fd = -1;
                    }

                    struct timeval tv;
                    tv.tv_sec = 20;
                    tv.tv_usec = 0;
                    setsockopt(client_sock_fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(struct timeval));
                    telnet.client_fd = client_sock_fd;
                    telnet.client_timeout = PKG_AGILE_TELNET_CLIENT_DEFAULT_TIMEOUT;
                    client_tick_timeout = rt_tick_get() + rt_tick_from_millisecond(telnet.client_timeout * 60000);

                    rt_base_t level = rt_hw_interrupt_disable();
                    rt_ringbuffer_reset(&(telnet.tx_ringbuffer));
                    rt_hw_interrupt_enable(level);

                    telnet.isconnected = 1;
                #ifdef FINSH_USING_MSH
                    msh_exec("version", strlen("version"));
                #endif
                    rt_kprintf(FINSH_PROMPT);
                }
            }

            // 客户端事件
            if(telnet.client_fd >= 0)
            {
                if(FD_ISSET(telnet.client_fd, &exceptset))
                {
                    telnet.isconnected = 0;
                    close(telnet.client_fd);
                    telnet.client_fd = -1;
                }
                else if(FD_ISSET(telnet.client_fd, &readset))
                {
                    int recv_len = recv(telnet.client_fd, recv_buf, RECV_BUF_LEN, MSG_DONTWAIT);
                    if(recv_len <= 0)
                    {
                        telnet.isconnected = 0;
                        close(telnet.client_fd);
                        telnet.client_fd = -1;
                    }
                    else
                    {
                        process_rx(&telnet, recv_buf, recv_len);
                        rt_sem_release(telnet.read_notice);

                        client_tick_timeout = rt_tick_get() + rt_tick_from_millisecond(telnet.client_timeout * 60000);
                    }
                }
                else if(FD_ISSET(telnet.client_fd, &writeset))
                {
                    int send_len = process_tx(&telnet);
                    if(send_len > 0)
                    {
                        client_tick_timeout = rt_tick_get() + rt_tick_from_millisecond(telnet.client_timeout * 60000);
                    }
                    else  
                        rt_thread_mdelay(50);
                }
            }
        }

        if(telnet.client_fd >= 0)
        {
            if((rt_tick_get() - client_tick_timeout) < (RT_TICK_MAX / 2))
            {
                telnet.isconnected = 0;
                close(telnet.client_fd);
                telnet.client_fd = -1;
            }
        }
    }

_telnet_restart:
    telnet.isconnected = 0;
    if(telnet.server_fd >= 0)
    {
        close(telnet.server_fd);
        telnet.server_fd = -1;
    }
    if(telnet.client_fd >= 0)
    {
        close(telnet.client_fd);
        telnet.client_fd = -1;
    }

    rt_thread_mdelay(10000);
    goto _telnet_start;

}

#ifdef RT_USING_DEVICE_OPS
    static struct rt_device_ops _ops = {
        telnet_init,
        telnet_open,
        telnet_close,
        telnet_read,
        telnet_write,
        telnet_control
    };
#endif

static int telnet_device_register(void)
{
    rt_memset(&telnet, 0, sizeof(struct telnet_session));

    /* register telnet device */
    telnet.device.type = RT_Device_Class_Char;
#ifdef RT_USING_DEVICE_OPS
    telnet.device.ops = &_ops;
#else
    telnet.device.init = telnet_init;
    telnet.device.open = telnet_open;
    telnet.device.close = telnet_close;
    telnet.device.read = telnet_read;
    telnet.device.write = telnet_write;
    telnet.device.control = telnet_control;
#endif

    /* no private */
    telnet.device.user_data = RT_NULL;

    /* register telnet device */
    rt_device_register(&telnet.device, "telnet", RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_STREAM);

    telnet.isconnected = 0;
    telnet.server_fd = -1;
    telnet.client_fd = -1;
    telnet.client_timeout = PKG_AGILE_TELNET_CLIENT_DEFAULT_TIMEOUT;

    rt_console_set_device("telnet");
    return RT_EOK;
}
INIT_BOARD_EXPORT(telnet_device_register);

static int telnet_module_init(void)
{
    rt_uint8_t *ptr = rt_malloc(PKG_AGILE_TELNET_RX_BUFFER_SIZE);
    RT_ASSERT(ptr != RT_NULL);
    rt_ringbuffer_init(&telnet.rx_ringbuffer, ptr, PKG_AGILE_TELNET_RX_BUFFER_SIZE);

    telnet.rx_ringbuffer_lock = rt_mutex_create("telnet_rx", RT_IPC_FLAG_FIFO);
    RT_ASSERT(telnet.rx_ringbuffer_lock != RT_NULL);

    ptr = rt_malloc(PKG_AGILE_TELNET_TX_BUFFER_SIZE);
    RT_ASSERT(ptr != RT_NULL);
    rt_ringbuffer_init(&telnet.tx_ringbuffer, ptr, PKG_AGILE_TELNET_TX_BUFFER_SIZE);

    telnet.read_notice = rt_sem_create("telnet_rx", 0, RT_IPC_FLAG_FIFO);
    RT_ASSERT(telnet.read_notice != RT_NULL);

    rt_thread_t tid = rt_thread_create("telnet", telnet_thread, RT_NULL, PKG_AGILE_TELNET_THREAD_STACK_SIZE, PKG_AGILE_TELNET_THREAD_PRIORITY, 100);
    RT_ASSERT(tid != RT_NULL);
    rt_thread_startup(tid);

    return RT_EOK;
}
INIT_ENV_EXPORT(telnet_module_init);

static int telnet_client_timeout(int argc, char **argv)
{
    if(argc == 1)
    {
        rt_kprintf("telnet client timeout:%d min\r\n", telnet.client_timeout);
    }
    else if(argc == 2)
    {
        int timeout = atoi(argv[1]);
        if(timeout <= 0)
        {
            rt_kprintf("telnet client timeout must be greater than 0.");
        }
        else
        {
            telnet.client_timeout = timeout;
            rt_kprintf("set telnet client timeout success.\r\n");
        }
    }
    else
    {
        rt_kprintf("Usage:\r\n");
        rt_kprintf("telnet_client_timeout           - get telnet client timeout\r\n");
        rt_kprintf("telnet_client_timeout timeout   - set telnet client timeout\r\n");
    }
  
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(telnet_client_timeout, telnet_ctm, telnet client teimeout);

#endif /* PKG_USING_AGILE_TELNET */
