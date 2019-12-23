
#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__

#include "http_parser.h"

#include <pthread.h>
#include <stdatomic.h>
#include <semaphore.h>
#include <netinet/in.h>

struct http_server_tag;
struct http_processor_tag;

typedef struct http_processor_tag {
    int sock;
    pthread_t thread;
    struct http_processor_tag * next;
    struct http_server_tag * server;
    struct sockaddr_in client_addr;
    int closed;
} http_processor_t;

typedef struct http_server_tag {
    sem_t processor_cleanup;
    int completed;
    int sock;
    pthread_t listen_thread;
    pthread_t cleanup_thread;
    int wait_queue;
    pthread_mutex_t mutex;
    struct http_processor_tag * processors;
    atomic_int processor_count;

    uint8_t * motion;
    size_t motion_size;

    uint8_t * frame;
    size_t frame_size;

    uint8_t * config;
    size_t config_size;

} http_server_t;

int http_server_create(http_server_t * server, int portno);
int http_server_destroy(http_server_t * server);

int http_server_frame_jpeg(http_server_t * server, uint8_t * data, size_t length);
int http_server_motion(http_server_t * server, uint8_t * data, size_t length);
int http_server_config(http_server_t * server, uint8_t * data, size_t length);

#endif