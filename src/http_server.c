
#include "http_server.h"

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

#include <interface/vcos/vcos.h>
#include <interface/vcos/vcos_mutex.h>
#include <interface/vcos/vcos_semaphore.h>
#include <interface/mmal/mmal_logging.h>

const char invalid_request[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
const char ok_message[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok";
const char response_header_format[] = "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n";

const char mime_image_jpeg[] = "image/jpeg";
const char mime_text_plain[] = "text/plain";
const char mime_octet_stream[] = "application/octet-stream";
const char mime_motion_jpeg[] = "video/x-motion-jpeg";

const char route_ping[] = "/ping";
const char route_config[] = "/config";
const char route_frame[] = "/frame.jpg";
const char route_frame_raw[] = "/frame.raw";
const char route_motion[] = "/motion.bin";
const char route_video[] = "/video.jpg";

static int parser_message_complete(http_parser * parser) {
    // http_processor_t * p = (http_processor_t*)parser->data;

    return 0;
}

struct __buffer {
    const char * data;
    size_t length;
};

int processor_get_url(http_parser * parser, const char * at, size_t length) {
    struct __buffer * buf = (struct __buffer *)parser->data;

    buf->data = at;
    buf->length = length;

    return 0;
}

int is_route(const char * route, struct __buffer * buf) {
    return strlen(route) == buf->length && strncmp(route, buf->data, buf->length) == 0;
}

// maximum requeset size: 4096
void * processor_thread(void * user) {
    fprintf(stderr, "starting processor thread.\n");
    http_processor_t * p = (http_processor_t*)user;
    http_server_t * server = p->server;

    http_parser parser;
    http_parser_settings parser_settings;

    struct __buffer url_buf = {
        .data = NULL,
        .length = 0
    };

    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = (void*)&url_buf;
    http_parser_settings_init(&parser_settings);
    // parser_settings.on_message_complete = parser_message_complete;
    parser_settings.on_url = processor_get_url;

    static const int bufsiz = 4096;
    char buffer[bufsiz];
    int ps;


    fprintf(stderr, "reading from socket\n");

    int bytes_read = recv(p->sock, buffer, bufsiz-1, MSG_NOSIGNAL);

    fprintf(stderr, "read %d bytes\n", bytes_read);
    if (bytes_read < 0) {
        perror("error on socket recv");
        goto cleanup;
    } else if (bytes_read == 0) {
        fprintf(stderr, "error or no bytes read from socket\n");
        goto cleanup;
    }
    
    buffer[bytes_read] = '\0';
    fprintf(stderr, "read: %s", buffer);

    if ((ps = http_parser_execute(&parser, &parser_settings, buffer, bytes_read)) < bytes_read) {
        fprintf(stderr, "error: parsed bytes %d of %d\n", ps, bytes_read);
        const char * msg = "invalid http request";
        send_http_response(p->sock,
            HTTP_STATUS_BAD_REQUEST, mime_text_plain,
            msg, strlen(msg));
        // TODO: send 4XX error
        goto cleanup;
    }

    fprintf(stderr, "parsed: %d\n", parser.method);

    if (parser.method != HTTP_GET) {
        // TODO: send 4XX error
        const char * msg = "method not supported";
        send_http_response(p->sock, 
            HTTP_STATUS_METHOD_NOT_ALLOWED, 
            mime_text_plain,
            msg, strlen(msg));

        goto cleanup;
    }
    
    if (is_route(route_ping, &url_buf)) {
        send_http_response(p->sock, HTTP_STATUS_OK, mime_text_plain, url_buf.data, url_buf.length);
    } else if (is_route(route_config, &url_buf)) {
        pthread_mutex_lock(&p->server->mutex);
        send_http_response(p->sock, HTTP_STATUS_OK, mime_text_plain, p->server->config, p->server->config_size);
        pthread_mutex_unlock(&p->server->mutex);
    } else if (is_route(route_frame, &url_buf)) {
        pthread_mutex_lock(&p->server->mutex);
        send_http_response(p->sock, HTTP_STATUS_OK, mime_image_jpeg, p->server->frame, p->server->frame_size);
        pthread_mutex_unlock(&p->server->mutex);
    } else if (is_route(route_motion, &url_buf)) {
        pthread_mutex_lock(&p->server->mutex);
        send_http_response(p->sock, HTTP_STATUS_OK, mime_octet_stream, p->server->motion, p->server->motion_size);
        pthread_mutex_unlock(&p->server->mutex);
    } else {
        send_http_response(p->sock, HTTP_STATUS_NOT_FOUND, mime_text_plain, "not found\n", 10);
    }


cleanup:

    // shutdown(p->sock, SHUT_RDWR);
    close(p->sock);
    p->closed = 1;

    // fprintf(stderr, "processor cleanup %x\n", server);
    fprintf(stderr, "server %d\n", server->processor_count);

    sem_post(&server->processor_cleanup);

    return NULL;
}

int send_http_response(int sock, int status, const char * content_type, const char * data, size_t length) {
    const char * status_name = http_status_str(status);
    int ret = 0;

    char buf[512];

    snprintf(buf, sizeof(buf), response_header_format 
        ,status  // status
        ,status_name              // status message
        ,content_type                 // content-type
        ,length     // content-length
    );

    int s = 0;
    s = send(sock, buf, strlen(buf), 0);
    if(s < 0) {
        return s;
    }
    ret += s;

    if (data != NULL) {
        s = send(sock, data, length, 0);
        if(s < 0) {
            return s;
        }
    }

    return ret + s;
}

void * cleanup_thread(void * user) {
    http_server_t * server = (http_server_t*)user;

    int done = 0;

    while(!done) {
        fprintf(stderr, "cleanup thread waiting\n");
        sem_wait(&server->processor_cleanup);

        pthread_mutex_lock(&server->mutex);

        fprintf(stderr, "before cleanup: %d\n", server->processor_count);

        for(http_processor_t ** l = &server->processors; *l;) {
            fprintf(stderr, "in cleanup loop\n");
            http_processor_t * p = *l;
            
            if (p->closed || server->completed) {
                fprintf(stderr, "removing socket: %d\n", p->sock);
                if (!p->closed) {
                    close(p->sock);
                }
                // unlink this node
                *l = p->next;
                p->next = NULL;

                free(p);
                p->server->processor_count--;
            } else {
                l = &p->next;
            }
        }
        fprintf(stderr, "after cleanup: %d\n", server->processor_count);

        done = server->completed;

        pthread_mutex_unlock(&server->mutex);
    }
    return NULL;
}

void * listen_thread(void * user) {
    http_server_t * server = (http_server_t*)user;
    struct sockaddr_in cli_addr;
    memset(&cli_addr, 0, sizeof(cli_addr));
    unsigned int clilen = sizeof(cli_addr);
    int new_socket;

    while(listen(server->sock, server->wait_queue) == 0) {
        fprintf(stderr, "accepting socket.\n");

        memset(&cli_addr, 0, sizeof(cli_addr));
        new_socket = accept(server->sock, (struct sockaddr*)&cli_addr, &clilen);
        if (new_socket < 0) {
            perror("could not create new socket");
            break;
        }

        // spawn a processor thread
        pthread_mutex_lock(&server->mutex);
        http_processor_t * proc = (http_processor_t*)malloc(sizeof(http_processor_t));
        proc->server = server;
        proc->sock = new_socket;
        proc->client_addr = cli_addr;
        proc->closed = 0;

        proc->next = server->processors;
        server->processors = proc;

        server->processor_count++;
        pthread_mutex_unlock(&server->mutex);

        fprintf(stderr, "client connected: %s:%d\n", inet_ntoa(cli_addr.sin_addr), cli_addr.sin_port);

        if(pthread_create(&proc->thread, NULL, processor_thread, proc) != 0) {
            perror("could not create processor thread");
            break;
        }

        pthread_detach(proc->thread);
    }

    pthread_mutex_lock(&server->mutex);
    server->completed = 1;

    for (http_processor_t * proc = server->processors; proc;) {
        // shutdown(proc->sock, SHUT_RDWR);
        close(proc->sock);

        http_processor_t * t = proc;
        proc = proc->next;
        server->processor_count--;

        free(t);
    }
    server->processors = NULL;
    pthread_mutex_unlock(&server->mutex);

    sem_post(&server->processor_cleanup);

    return NULL;
}

int http_server_destroy(http_server_t * server) {
    if (shutdown(server->sock, SHUT_RDWR) != 0) {
        perror("could not shut down socket");
    }
    close(server->sock);

    pthread_join(server->listen_thread, NULL);
    pthread_join(server->cleanup_thread, NULL);
    
    pthread_mutex_destroy(&server->mutex);
    sem_destroy(&server->processor_cleanup);

    if (server->config != NULL) {
        free(server->config);
        server->config = NULL;
        server->config_size = 0;
    }
    if (server->frame != NULL) {
        free(server->frame);
        server->frame = NULL;
        server->frame_size = 0;
    }
    if (server->motion != NULL) {
        free(server->motion);
        server->motion = NULL;
        server->motion_size = 0;
    }

    return 0;
}

int http_server_create(http_server_t * server, int portno) {
    struct sockaddr_in serv_addr;
    int opt = 1;
    int sock = -1;
    int mutex_created = 0;
    int semaphore_created = 0;
    int listen_thread_started = 0;
    int cleanup_thread_started = 0;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("could not create socket.");
        goto error;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
        &opt, sizeof(opt)) != 0) 
    {
        perror("could not set socket options");
        goto error;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    if (bind(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("could not bind socket");
        goto error;
    }

    server->sock = sock;
    server->wait_queue = 3;
    server->processors = NULL;
    server->completed = 0;
    server->processor_count = 0;
    server->config = NULL;
    server->config_size = 0;
    server->frame = NULL;
    server->frame_size = 0;
    server->motion = NULL;
    server->motion_size = 0;

    if(pthread_mutex_init(&server->mutex, NULL) != 0) {
        perror("could not create http server mutex");
        goto error;
    }
    mutex_created = 1;
    if(sem_init(&server->processor_cleanup, 0, 0) != 0) {
        perror("could not create http server semaphore");
        goto error;
    }

    semaphore_created = 1;
    if (pthread_create(&server->listen_thread, NULL, listen_thread, (void*)server) != 0) {
        perror("could not start listener thread");
        goto error;
    }
    listen_thread_started = 1;
    if (pthread_create(&server->cleanup_thread, NULL, cleanup_thread, (void*)server) != 0) {
        perror("could not start cleanup thread");
        goto error;
    }
    cleanup_thread_started = 1;


    return 0;
error:
    if (sock > 0) {
        close(sock);
    }
    server->completed = 1;
    if (listen_thread_started) {
        pthread_join(server->listen_thread, NULL);
    }
    if (cleanup_thread_started) {
        sem_post(&server->processor_cleanup);
        pthread_join(server->cleanup_thread, NULL);
    }
    if (semaphore_created) {
        sem_close(&server->processor_cleanup);
        sem_destroy(&server->processor_cleanup);
    }
    if (mutex_created) {
        pthread_mutex_destroy(&server->mutex);
    }

    return -1;
}

int http_server_config(http_server_t * server, uint8_t * data, size_t length) {
    pthread_mutex_lock(&server->mutex);

    if (server->config != NULL) {
        free(server->config);
    }

    server->config = (uint8_t*)malloc(length);
    memcpy(server->config, data, length);
    server->config_size = length;

    pthread_mutex_unlock(&server->mutex);
    return 0;
}


int http_server_frame_jpeg(http_server_t * server, uint8_t * data, size_t length) {
    pthread_mutex_lock(&server->mutex);

    if (server->frame != NULL) {
        free(server->frame);
    }

    server->frame = (uint8_t*)malloc(length);
    memcpy(server->frame, data, length);
    server->frame_size = length;

    pthread_mutex_unlock(&server->mutex);

    // TODO: trigger a condition variable or semaphore
    return 0;
}


int http_server_motion(http_server_t * server, uint8_t * data, size_t length) {
    pthread_mutex_lock(&server->mutex);

    if (server->motion != NULL) {
        free(server->motion);
    }

    server->motion = (uint8_t*)malloc(length);
    memcpy(server->motion, data, length);
    server->motion_size = length;

    pthread_mutex_unlock(&server->mutex);
    return 0;
}