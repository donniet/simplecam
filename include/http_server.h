
#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__

#include "http_parser.h"

#include <pthread.h>
#include <stdatomic.h>
#include <interface/vcos/vcos_mutex.h>


struct http_server_tag;
struct http_processor_tag;

typedef struct http_processor_tag {
    int sock;
    pthread_t thread;
    struct http_processor_tag * next;
    struct http_server_tag * server;
    http_parser parser;

} http_processor_t;

typedef struct http_server_tag {
    int completed;
    int sock;
    pthread_t listen_thread;
    int wait_queue;
    VCOS_MUTEX_T mutex;
    struct http_processor_tag * processors;
    atomic_int processor_count;
} http_server_t;

int http_server_create(http_server_t * server, int portno);
int http_server_destroy(http_server_t * server);

int http_server_frame_jpeg(uint8_t * data, size_t length);
int http_server_motion(uint8_t * data, size_t length);
int http_server_config(uint8_t * data, size_t length);

#endif