
#ifndef __SERVER_H__
#define __SERVER_H__

#include "interface/vcos/vcos_mutex.h"

#include <stdatomic.h>

struct socket_list_tag;

typedef struct buffer_tag {
    uint8_t * data;
    size_t length;
} buffer_t;

typedef struct server_tag {
    int socketfd;
    int wait_queue;
    atomic_int socket_count;
    int completed;
    struct socket_list_tag * sockets;
    struct buffer_tag buffer;
    pthread_t listen_thread;
    
    VCOS_MUTEX_T mutex;

    VCOS_SEMAPHORE_T write_ready;
    VCOS_SEMAPHORE_T write_complete;
} server_t;

typedef struct socket_list_tag {
    int socket;
    pthread_t thread;
    server_t * server;
    int completed;
    struct socket_list_tag * next;
} socket_list_t;



int server_write(server_t * server, uint8_t * data, size_t length);
int server_create(server_t * server, int portno);
int server_close(server_t * server);


#endif