/*
  * A generic channel implementation
  *
  * Copyright (C) 2014 rainfly123 <xiechc@gmail.com>
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation; either version 2 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program; if not, write to the Free Software
  * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
  *
  */
#include <string.h>
#include <errno.h>
#include "channel.h"
#include "util.h"
#include "log.h"
#include "kfifo.h"
#include "udp.h"
#include "tcp.h"
#include "list.h"
#include "media.h"

//used for store all of the channels
List_t all_channels;

static const char *protocol[4] = {
    "udp://",
    "tcp://", 
    "http://", 
    "rtmp://"
};
/*
@brief just inited the list , which contains all of the channels
*/
void clive_init_channel(void)
{
    all_channels.count = 0;
    all_channels.head = NULL;
    all_channels.tail = NULL;
}

static void udp_close(struct con * conn)
{
    ASSERT(conn != NULL);
    close(conn->skt);
    conn->done = true;
}
static int udp_recv(struct con *conn)
{
    ssize_t n = 0, w = 0;
    Channel *channel;
    uint8_t buffer[MAX_UDP_LEN];
    uint32_t total;

    ASSERT(conn != NULL);
    channel = conn->ctx;
    if (channel->input_media_type == Unknown) { 
        for (;;) {
        //////////////////////////
           //recv...
        n = clive_udp_recv(conn->skt, NULL, NULL, &buffer, sizeof(buffer));
        if (n > 0) {
            if (buffer[0] == 'F') {
                channel->input_media_type = FLV;
            }else if (buffer[0] == 0x47) {
                channel->input_media_type = TS;
            } else {
                log_error("unsupported stream format");
            }
        } else {

            if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return CL_OK;
            } else {
                conn->err = errno;
                log_error("recv on %d failed: %s", conn->skt, strerror(errno));
                return CL_ERROR;
            }
        }
        w = kfifo_put(channel->buffer, buffer, n); 
        if (w < n){
            log_error("buffer overflow");
        }
        if (channel->input_media_type == TS) {
            clive_media_setype(channel->flv_media, TS2FLV, TS);
            clive_media_setype(channel->ts_media, TS2TS, TS);
        }
        if (channel->input_media_type == FLV) {
            clive_media_setype(channel->flv_media, FLV2FLV, FLV);
            clive_media_setype(channel->ts_media, FLV2TS, FLV);
        }
        clive_media_attach(channel->flv_media, channel->ts_media, channel->buffer);
        break;
       }
       if (channel->input_media_type == TS)
           log_debug(LOG_INFO, "stream format detected MPEGTS");
       else
           log_debug(LOG_INFO, "stream format detected FLV");
    }
    for (;;) {
        //////////////////////////
           //recv...
        n = clive_udp_recv(conn->skt, NULL, NULL, &buffer, sizeof(buffer));
        if (n > 0) {
            total = 0;
            do {
                w = kfifo_put(channel->buffer, (buffer + total), (n - total)); 
                total += w;
            } while (total < n);
            return CL_OK;
        } else {
            if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return CL_OK;
            } else {
                conn->err = errno;
                log_error("recv on %d failed: %s", conn->skt, strerror(errno));
                return CL_ERROR;
            }
        }
    }
}

static void tcp_close(struct con * conn)
{
    ASSERT(conn != NULL);
    close(conn->skt);
    clive_free(conn);
}

static int tcp_recv(struct con *conn)
{
    ssize_t n;

    ASSERT(conn != NULL);

    for (;;) {
        //////////////////////////
           //recv...
        if (n > 0) {
            return CL_OK;
        }

        if (n == 0) {
            return CL_CLOSE;
        }

        if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return CL_OK;
        } else {
            conn->err = errno;
            log_error("recv on %d failed: %s", conn->skt, strerror(errno));
            return CL_ERROR;
        }
    }
}

static void listener_close(struct con * conn)
{
    ASSERT(conn != NULL);
    close(conn->skt);
    conn->done = true;
}

static int listener_recv(struct con * conn)
{
    ASSERT(conn != NULL);
    int client;
    struct sockaddr_in from;
    uint32_t  slen = sizeof(from);
    struct con *new;
    struct event_base *evb = conn->evb;

    client = accept(conn->skt, (struct sockaddr *)&from, &slen);
    if (client > 0) {
        clive_set_nonblocking(client);
        clive_set_sndbuf(client, 512*1024);
        new = clive_calloc(1, sizeof(struct con));
        new->skt = client;
        new->type = tHTTP;
        new->evb = evb;
        new->ctx = conn->ctx;//pointer channel

        new->recv = &tcp_recv;
        new->close = &tcp_close;
        event_add_conn(evb, new);
        event_del_out(evb, new);
    }
    return CL_OK;
}


/*
*@brief  create a new channel
*@param evb a pointer to event_base, which this channel will run in
*@param url channels's input url
*    url: could be:
*         1) http://x.x.x.x:80/xx //cliveserver is client
*         2) tcp://x.x.x.x:9090  //cliveserver is tcp server
*         3) udp://x.x.x.x:8080  //cliveserver is udp server
*         4) rtmp://x.x.x.x:1935/live/cnv //cliveserver is client
*@return a pointer to new alloced channel
*        NULL for failed
*/

Channel * clive_new_channel(struct event_base *evb, char *url, char *name)
{
    Channel *channel;
    int val;
    int status;
    char *port;
    char *ip = url;
    char *loc;
    char ip_addr[64];
    uint16_t sport = 80;
    char location[32];
    int input_protocol = -1;
    size_t i = 0;
    struct sockinfo sock;
    int skt;

    ASSERT(evb != NULL);
    ASSERT(url != NULL);

    memset(ip_addr, 0, sizeof(ip_addr));
    memset(location, 0, sizeof(location));

    channel = clive_calloc(1, sizeof(Channel));
    ASSERT(channel != NULL);

    for (; i < sizeof(protocol) / sizeof(char *); i++)
    {
        if (!strncmp(url, protocol[i], strlen(protocol[i])))
        {
            input_protocol = i;
            break;
        }
    }
    switch (input_protocol)
    {
        case HTTP:
        {
            ip += strlen(protocol[input_protocol]);
            port = strchr(ip, ':');
            if (port != NULL) {
                strncpy(ip_addr, ip, port - ip);
                port += 1;
                sport = clive_atoi(port, 5);
            } else {
                port = strchr(ip, '/');
                strncpy(ip_addr, ip, port - ip);
            }
            loc = strchr(port, '/');
            if (loc != NULL) {
                strncpy(location, loc, sizeof(location));
            }
            val = clive_resolve(ip_addr, sport, &sock);
            break;
        }
        case UDP:
        case TCP:
        {
            ip += strlen(protocol[input_protocol]);
            port = strchr(ip, ':');
            if (port != NULL) {
                strncpy(ip_addr, ip, port - ip);
                port += 1;
                sport = clive_atoi(port, 5);
            } else {
                strcpy(ip_addr, ip);
                sport = 9090; //default value
            }
            break;
        }
    }
    if (input_protocol == TCP) {
        skt = clive_tcp_socket();
        clive_set_tcpnodelay(skt);
        clive_set_nonblocking(skt);
        clive_set_sndbuf(skt, 64*1024);
        clive_set_rcvbuf(skt, 64*1024);
        clive_tcp_bind(skt, ip_addr, sport);
        clive_tcp_listen(skt, 3);
        channel->connection.recv = &listener_recv;
        channel->connection.close = &listener_close;
    }
    if (input_protocol == UDP) {
        skt = clive_udp_socket();
        clive_set_nonblocking(skt);
        clive_set_sndbuf(skt, 64*1024);
        clive_set_rcvbuf(skt, 64*1024);
        clive_udp_bind(skt, ip_addr, sport);
        channel->connection.recv = &udp_recv;
        channel->connection.close = &udp_close;
    }
    if (input_protocol == HTTP) {
       //.....
    }
    channel->buffer = kfifo_alloc(4 * 1024 * 1024); //4Mbytes
    channel->input_protocol = input_protocol;
    channel->input_media_type = -1;
    channel->evb = evb;
    channel->connection.skt = skt;
    channel->connection.evb = evb;
    //channel->connection.send = &conn_send;
    channel->connection.ctx = channel;
    log_debug(LOG_INFO, "clive_new_channel ip:%s port:%d location:%s", ip_addr, sport, location);

    return channel;
}

/*
*@brief start channel*
*@param a pointer to channel
*@return 0 succeeded, -1 else
*/
int clive_channel_start(Channel * channel)
{
    int val;
    struct event_base *evb;

    ASSERT(channel != NULL);

    evb = channel->evb;
    val = event_add_conn(evb, &channel->connection);
    if (val != 0) {
        return val;
    }

    if ((channel->input_protocol == TCP) || (channel->input_protocol == UDP))
        val = event_del_out(evb, &channel->connection);

    return val; 
}

/*
*@brief stop channel*
*@param a pointer to channel
*@return 0 succeeded, -1 else
*/
int clive_channel_stop(Channel *channel)
{
    ASSERT(channel != NULL);
    return event_del_conn(channel->evb, &channel->connection);
}

/*
*@brief set channels outputs*
*@param a pointer to channel
*@param output url
*       url could be: 
*              "http://127.0.0.1:80/tvb_ts", 
*              "http://127.0.0.1:80/tvb_flv",
*              "http://127.0.0.1:80/tvb.m3u8"
*              "rtmp://127.0.0.1:1935/live/tvb"
*@return 0 succeeded, -1 failed
*/
int clive_channel_add_output(Channel * channel, char *url)
{
    char *protocol;

    ASSERT(channel != NULL);
    ASSERT(url != NULL);
   
    protocol = strstr(url, "_flv");
    if (protocol != NULL) {
        channel->flv_media =  clive_media_create(Unknown, Unknown);
        return 0;
    }
    else {
         protocol = strstr(url, "rtmp:");
         if (protocol != NULL) {
             channel->flv_media =  clive_media_create(Unknown, Unknown);
             return 0;
         }
    }

    protocol = strstr(url, "_ts");
    if (protocol != NULL) {
        channel->ts_media =  clive_media_create(Unknown, Unknown);
        return 0;
    }
    else {
         protocol = strstr(url, ".m3u8");
         if (protocol != NULL) {
             channel->ts_media =  clive_media_create(Unknown, Unknown);
             return 0;
         }
    }

    log_error("clive_channel_add_output: unsupported output protocol");
    return -1;
}

bool clive_channel_is_existed(const char *name)
{
    ListIterator_t iterator;
    Channel *p;
    int val;

    if (name == NULL) {
        return false;
    }

    ListIterator_Init(iterator, &all_channels); 

    for ( ; ListIterator_MoreEntries(iterator); ListIterator_Next(iterator))
    {
        p = ListIterator_Current(iterator);
        val = strncmp(p->channel_name, name, strlen(p->channel_name));
        if (val == 0)
            return true;
    }
    return false;
}

Channel * clive_channel_find(const char *name)
{
    ListIterator_t iterator;
    Channel *p;
    int val;

    if (name == NULL) {
        return false;
    }

    ListIterator_Init(iterator, &all_channels); 

    for ( ; ListIterator_MoreEntries(iterator); ListIterator_Next(iterator))
    {
        p = ListIterator_Current(iterator);
        val = strncmp(p->channel_name, name, strlen(p->channel_name));
        if (val == 0)
            return p;
    }
    return NULL;
}

bool clive_channel_add(Channel *channel) {

    if (channel == NULL) {
        return false;
    }
    return ListAdd(&all_channels, channel);
}
