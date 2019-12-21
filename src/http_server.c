
#include "http_server.h"

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

#include <interface/vcos/vcos.h>
#include <interface/vcos/vcos_mutex.h>
#include <interface/mmal/mmal_logging.h>

void parse_request(http_processor_t * p, char * buf, int siz) {
    // get the first line
    
}

static int parser_message_complete(http_parser * parser) {
    http_processor_t * p = (http_processor_t*)parser->data;

    return 0;
}

// maximum requeset size: 4096
void * processor_thread(void * user) {
    fprintf(stderr, "starting processor thread.\n");
    http_processor_t * p = (http_processor_t*)user;

    http_parser parser;
    http_parser_settings parser_settings;

    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = (void*)p;
    http_parser_settings_init(&parser_settings);
    // parser_settings.on_message_complete = parser_message_complete;

    static const int bufsiz = 4096;
    char buffer[bufsiz];

    fprintf(stderr, "reading from socket\n");

    int s = recv(p->sock, buffer, bufsiz-1, 0);
    // int s = read(p->sock, buffer, bufsiz-1);
    if (s <= 0) {
        fprintf(stderr, "error or no bytes read from socket\n");
        return NULL;
    }

    buffer[s] = '\0';
    fprintf(stderr, "read: %s", buffer);

    int ps;
    if ((ps = http_parser_execute(&parser, &parser_settings, buffer, s)) < s) {
        fprintf(stderr, "error: parsed bytes %d of %d\n", ps, s);
        // TODO: send 4XX error
        return NULL;
    }

    fprintf(stderr, "parsed: %d\n", parser.method);

    if (parser.method != HTTP_GET) {
        // TODO: send 4XX error
        return NULL;
    }


    shutdown(p->sock, SHUT_RDWR);
    close(p->sock);

    vcos_mutex_lock(&p->server->mutex);
    // remove ourselves from the processor list
    for(http_processor_t ** l = &p->server->processors; *l;) {
        http_processor_t * pp = *l;

        if (pp == p) {
            // this is us, cut us out;
            *l = p->next;
            p->next = NULL;
            free(p);
            break;
        }
    }
    vcos_mutex_unlock(&p->server->mutex);

    return NULL;
}

void * listen_thread(void * user) {
    http_server_t * server = (http_server_t*)user;

    struct sockaddr_in cli_addr;
    unsigned int clilen = sizeof(cli_addr);

    while(listen(server->sock, server->wait_queue) == 0) {
        fprintf(stderr, "accepting socket.\n");
        int new_socket = accept(server->sock, (struct sockaddr*)&cli_addr, &clilen);
        if (new_socket < 0) {
            perror("could not create new socket");
            break;
        }

        // spawn a processor thread
        vcos_mutex_lock(&server->mutex);
        http_processor_t * proc = (http_processor_t*)malloc(sizeof(http_processor_t));
        proc->next = server->processors;
        server->processors = proc;
        proc->server = server;
        server->processor_count++;
        vcos_mutex_unlock(&server->mutex);

        if(pthread_create(&proc->thread, NULL, processor_thread, proc) != 0) {
            perror("could not create processor thread");
            break;
        }
    }

    vcos_mutex_lock(&server->mutex);

    server->completed = 1;

    for (http_processor_t * proc = server->processors; proc;) {
        shutdown(proc->sock, SHUT_RDWR);
        close(proc->sock);

        pthread_join(proc->thread, NULL);

        http_processor_t * t = proc;
        proc = proc->next;
        server->processor_count--;

        free(t);
    }

    vcos_mutex_unlock(&server->mutex);

    return NULL;
}

int http_server_destroy(http_server_t * server) {
    if (shutdown(server->sock, SHUT_RDWR) != 0) {
        perror("could not shut down socket");
    }
    close(server->sock);

    pthread_join(server->listen_thread, NULL);
    
    vcos_mutex_delete(&server->mutex);

    return 0;
}

int http_server_create(http_server_t * server, int portno) {
    struct sockaddr_in serv_addr;
    int opt = 1;
    int sock = -1;
    int mutex_created = 0;

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

    if(vcos_mutex_create(&server->mutex, "http_server_mutex") != VCOS_SUCCESS) {
        vcos_log_error("could not create http server mutex");
        goto error;
    }
    mutex_created = 1;

    if (pthread_create(&server->listen_thread, NULL, listen_thread, (void*)server) != 0) {
        perror("could not start listener thread");
        goto error;
    }


    return 0;
error:
    if (sock > 0) {
        close(sock);
    }
    if (mutex_created) {
        vcos_mutex_delete(&server->mutex);
    }

    return -1;
}