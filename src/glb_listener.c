/*
 * Copyright (C) 2008-2013 Codership Oy <info@codership.com>
 *
 * $Id$
 */

#include "glb_listener.h"
#include "glb_log.h"
#include "glb_limits.h"
#include "glb_cmd.h"

#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <poll.h>

#ifndef _GNU_SOURCE
#include <fcntl.h>
#endif /* _GNU_SOURCE */

typedef struct pollfd pollfd_t;

struct glb_listener
{
    const glb_cnf_t* cnf;
    glb_router_t* router;
    glb_pool_t*   pool;
    pthread_t     thread;
    int           sock;
};

static void*
listener_thread (void* arg)
{
    glb_listener_t* listener = arg;

    while (!glb_terminate) {
        int            ret;
        int            client_sock;
        glb_sockaddr_t client;
        socklen_t      client_size = sizeof(client);
        int            server_sock;
        glb_sockaddr_t server;

#ifdef _GNU_SOURCE
        client_sock = accept4(listener->sock,
                              (struct sockaddr*) &client, &client_size,
                              SOCK_CLOEXEC);
#else
        client_sock = accept (listener->sock,
                              (struct sockaddr*) &client, &client_size);
#endif /* _GNU_SOURCE */

        if (client_sock < 0 || glb_terminate) {
            if (client_sock < 0) {
                glb_log_error ("Failed to accept connection: %d (%s)",
                               errno, strerror (errno));
            }
            else {
                glb_log_debug ("Listener thread termonating.");
            }

            goto err;
        }

#ifndef _GNU_SOURCE
        (void) fcntl (client_sock, F_SETFD, FD_CLOEXEC);
#endif /* !_GNU_SOURCE */

        ret = glb_router_connect(listener->router, &client ,&server,
                                 &server_sock);
        if (server_sock < 0 && ret != -EINPROGRESS) {
            if (server_sock != -EMFILE)
                glb_log_error("Failed to connect to destination: %d (%s)",
                              -ret, strerror(-ret));
            goto err1;
        }

        assert (0 == ret || -EINPROGRESS == ret);

        glb_socket_setopt(client_sock, GLB_SOCK_NODELAY); // ignore error here

        ret = glb_pool_add_conn (listener->pool,
                                 client_sock, &client,
                                 server_sock, &server,
                                 0 == ret);
        if (ret < 0) {
            glb_log_error ("Failed to add connection to pool: "
                           "%d (%s)", -ret, strerror (-ret));
            goto err2;
        }

        if (listener->cnf->verbose) {
            glb_sockaddr_str_t ca = glb_sockaddr_to_str (&client);
            glb_sockaddr_str_t sa = glb_sockaddr_to_str (&server);
            glb_log_info ("Accepted connection from %s to %s\n", ca.str,sa.str);
        }
        continue;

    err2:
        assert (server_sock > 0);
        close  (server_sock);
        glb_router_disconnect (listener->router, &server, false);

    err1:
        assert (client_sock > 0);
        close  (client_sock);

    err:
        // to avoid busy loop in case of error
        if (!glb_terminate) usleep (100000);
    }

    return NULL;
}

glb_listener_t*
glb_listener_create (const glb_cnf_t* const cnf,
                     glb_router_t*    const router,
                     glb_pool_t*      const pool,
                     int              const sock)
{
    glb_listener_t* ret = NULL;

    if (listen (sock,
                cnf->max_conn ? cnf->max_conn : (1U << 14)/* 16K */)){
        glb_log_error ("listen() failed: %d (%s)", errno, strerror (errno));
        return NULL;
    }

    ret = calloc (1, sizeof (glb_listener_t));
    if (ret) {
        ret->cnf    = cnf;
        ret->router = router;
        ret->pool   = pool;
        ret->sock   = sock;

        if (pthread_create (&ret->thread, NULL, listener_thread, ret)) {
            glb_log_error ("Failed to launch listener thread: %d (%s)",
                           errno, strerror (errno));
            free (ret);
            ret = NULL;
        }
    }
    else
    {
        glb_log_error ("Failed to allocate listener object: %d (%s)",
                       errno, strerror (errno));
    }

    return ret;
}

extern void
glb_listener_destroy (glb_listener_t* listener)
{
    /* need to connect to own socket to break the accept() call */
    glb_sockaddr_t sockaddr;
    glb_sockaddr_init (&sockaddr, "0.0.0.0", 0);
    int socket = glb_socket_create (&sockaddr, 0);
    if (socket >= 0)
    {
        int err = connect (socket, (struct sockaddr*)&listener->cnf->inc_addr,
                           sizeof (listener->cnf->inc_addr));
        close (socket);
        if (err) {
            glb_log_error ("Failed to connect to listener socket: %d (%s)",
                           errno, strerror(errno));
            glb_log_error ("glb_listener_destroy(): failed to join thread.");
        }
        else {
            pthread_join (listener->thread, NULL);
        }
    }
    else {
        glb_log_error ("Failed to create socket: %d (%s)",
                       -socket, strerror(-socket));
    }
    free (listener);
}

