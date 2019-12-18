#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>

#include "interface/mmal/mmal_logging.h"

static void * client_thread(void * user) {
    fprintf(stderr, "client thread starting\n");
    socket_list_t * s = (socket_list_t*)user;

    while(!s->completed) {
        vcos_semaphore_wait(&s->server->write_ready);

        buffer_t * buf = &s->server->buffer;

        if (s->server->completed) {
            s->completed = 1;
        } else  {
            int w = write(s->socket, buf->data, buf->length);
            if(w < 0 || (size_t)w < buf->length) {
                // error, close socket
                s->completed = 1;
            } 
        }

        vcos_semaphore_post(&s->server->write_complete);
    }

    return NULL;
}

static void * server_write_thread(void * user) {
    // server_t * server = (server_t*)user;

    return NULL;
}

int server_write(server_t * server, uint8_t * data, size_t length) {
    vcos_mutex_lock(&server->mutex);
    server->buffer.data = data;
    server->buffer.length = length;

    // fprintf(stderr, "%s:%d: posting %d semaphores... ", __FILE__, __LINE__, server->socket_count);
    // post socket_count times
    for(int i = 0; i < server->socket_count; i++) 
        vcos_semaphore_post(&server->write_ready);

    // fprintf(stderr, " posted. waiting on %d semaphores...", server->socket_count);

    // wait for socket_count times on the complete
    for(int i = 0; i < server->socket_count; i++)
        vcos_semaphore_wait(&server->write_complete);

    // fprintf(stderr, " complete.\n");

    // clean up any completed socket
    // doing this here avoids any race conditions if another write 
    // is called because this is all under a lock
    // and we know the sockets are completed due to the wait
    for(socket_list_t ** l = &server->sockets; *l;) { 
        if ((*l)->completed) {
            fprintf(stderr, "socket completed, removing from list\n");
            if(close((*l)->socket))
                perror("close status");

            fprintf(stderr, "joining thread...\n");
            pthread_join((*l)->thread, NULL);
            fprintf(stderr, "thread joined.\n");

            // cut this one out of the list, this also advances the iterator
            *l = (*l)->next;
            // join the running thread
            free(*l);
            server->socket_count--;
        } else {
            // advance the iterator
            l = &(*l)->next;
        }
    }

    server->buffer.data = NULL;
    server->buffer.length = 0;
    vcos_mutex_unlock(&server->mutex);
    return 0;
}


static void * listen_thread(void * user) {
    fprintf(stderr, "listen thread start.\n");
    server_t * server = (server_t*)user;

    struct sockaddr_in cli_addr;
    unsigned int clilen = sizeof(cli_addr);

    while(listen(server->socketfd, server->wait_queue) >= 0) {
        int new_socket = accept(server->socketfd, (struct sockaddr*)&cli_addr, &clilen);

        if (new_socket < 0) {
            // TODO: we can probably continue from here, but just trying this out
            vcos_log_error("invalid socket accept");
            break;
        }

        fprintf(stderr, "socket accepted, starting client thread\n");

        vcos_mutex_lock(&server->mutex);
        socket_list_t * n = (socket_list_t*)malloc(sizeof(socket_list_t));
        n->socket = new_socket;
        n->next = server->sockets;
        n->server = server;
        n->completed = 0;
        server->sockets = n;
        server->socket_count++;
        vcos_mutex_unlock(&server->mutex);

        fprintf(stderr, "starting client thread.\n");


        if(pthread_create(&n->thread, NULL, client_thread, n) != 0) {
            // what to do?  crash horribly?
            vcos_log_error("could not spin up client thread, exiting");
            break;
        }
    }


    // now clean up the sockets
    vcos_mutex_lock(&server->mutex);

    server->completed = 1;

    // tell everybody that we are ready to move on
    // post socket_count times
    for(int i = 0; i < server->socket_count; i++) 
        vcos_semaphore_post(&server->write_ready);

    // wait for socket_count times on the complete
    for(int i = 0; i < server->socket_count; i++)
        vcos_semaphore_wait(&server->write_complete);

    // cleanup all the sockets
    for(socket_list_t * l = server->sockets; l;) {
        close(l->socket);
        pthread_join(l->thread, NULL);
        server->socket_count--;
        socket_list_t * t = l;
        l = l->next;
        free(t);
    }
    server->sockets = NULL;
    vcos_mutex_unlock(&server->mutex);

    fprintf(stderr, "listen thread end.\n");
    return NULL;
}

int server_close(server_t * server) {
    if (server->completed) {
        return 0;
    }

    // closing the main socket should kill the main thread
    close(server->socketfd);

    pthread_join(server->listen_thread, NULL);

    vcos_mutex_delete(&server->mutex);
    vcos_semaphore_delete(&server->write_ready);
    vcos_semaphore_delete(&server->write_complete);

    return 0;
}

int server_create(server_t * server, int portno) {
    signal(SIGPIPE, SIG_IGN);

    struct sockaddr_in serv_addr; 
    int opt = 1; 
    int socketfd = -1;

    if (vcos_mutex_create(&server->mutex, "simplecam_server") != VCOS_SUCCESS) {
        vcos_log_error("could not create server mutex");
        goto error;
    }

    server->wait_queue = 1;

    socketfd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketfd < 0) {
        vcos_log_error("error opening socket");
        goto error;
    }
    if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, 
                                                  &opt, sizeof(opt)) != 0) { 
        vcos_log_error("could not set socket options");
        goto error;
    } 

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    if (bind(socketfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        vcos_log_error("error binding socket to port %d", portno);
        perror("could not bind socket");
        goto error;
    }

    server->socket_count = 0;
    server->completed = 0;
    server->sockets = NULL;
    server->buffer.data = NULL;
    server->buffer.length = 0;

    if(vcos_semaphore_create(&server->write_ready, "simplecam_writeready", 0) != VCOS_SUCCESS) {
        vcos_log_error("could not create writeready semaphore");
        goto error;
    }
    if(vcos_semaphore_create(&server->write_complete, "simplecam_writecomplete", 0) != VCOS_SUCCESS) {
        vcos_log_error("could not create writeready semaphore");
        goto error;
    }

    int s = pthread_create(&server->listen_thread, NULL, listen_thread, (void*)server);
    if (s != 0) {
        vcos_log_error("could not create listener thread: %d", s);
        goto error;
    }


    server->socketfd = socketfd;

    return 0;

error:
    close(socketfd);

    return -1;
}