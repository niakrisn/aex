/* 
 * File:   main.c
 * Author: niakrisn
 *
 * Created on November 19, 2022, 11:53 AM
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include <ev.h>
#include <pthread.h>

#include <aex_ring.h>

typedef struct {
    ev_async                sw;

    ev_io                   ae;
    int                     sock;

    ev_async                aw;
    aex_ring_t             *ring;

    struct {
        ev_async           *aw;
        aex_ring_t         *ring;
        struct ev_loop     *loop;
    } mod;
} aex_worker_io_t;

typedef struct {
    ev_async                sw;

    ev_async                aw;
    aex_ring_t             *ring;

    struct {
        ev_async           *aw;
        aex_ring_t         *ring;
        struct ev_loop     *loop;
    } io;
} aex_worker_mod_t;

typedef struct {
    int                     fd;
    ev_io                   re;
    ev_io                   we;

    struct sockaddr_in      addr;

    struct {
        ssize_t             len;
        uint8_t             data[256];
        uint8_t             tmp[256];
    } buf;

    uint8_t                 data;
} aex_stream_t;

static struct ev_loop      *loops[2] = {0};

static void
aex_io_event(struct ev_loop * const el, ev_async * const w, int f) {
    aex_worker_io_t * const wc = ev_userdata(el);
    aex_stream_t    * const s = aex_ring_get(wc->ring);

    if (s == NULL) return;

    ev_io_start(el, &s->we);
}

static void
aex_mod_event(struct ev_loop * const el, ev_async * const w, int f) {
    aex_worker_mod_t * const wc = ev_userdata(el);
    aex_stream_t     * const s = aex_ring_get(wc->ring);

    if (s == NULL) return;

    memcpy(s->buf.tmp, s->buf.data, s->buf.len);
    for (int64_t i = s->buf.len - 1; i >= 0; i--)
        s->buf.data[(s->buf.len - 1) - i] = s->buf.tmp[i];

    if (aex_ring_put(wc->io.ring, s)) {
        fprintf(stderr, "WARNIGN: failed to queue mod: queue is full\n");
        ev_async_send(wc->io.loop, wc->io.aw);
        return;
    }

    ev_async_send(wc->io.loop, wc->io.aw);
}

static void
aex_stop_event(struct ev_loop * const el, ev_async * const w, int f) {
    ev_break(el, EVBREAK_ALL);
}

static void
aex_stream_read_event(struct ev_loop * const el, ev_io * const e, const int f) {
    aex_worker_io_t * const wc = ev_userdata(el);
    aex_stream_t    * const s = (void *) ((uint8_t *) e
            - offsetof(aex_stream_t, re));
    ssize_t                 len;

    len = read(s->fd, s->buf.data + s->buf.len,
            sizeof(s->buf.data) - s->buf.len);
    if (len < 0) {
        perror("WARNING: failed to read from socket");
        return;
    }

    s->buf.len += len;
    if (s->buf.len == 0) {
        ev_io_stop(el, &s->re);
        ev_io_stop(el, &s->we);
        close(s->fd);

        {
            char buf[INET_ADDRSTRLEN];
            fprintf(stdout, "INFO: close stream: %s:%u\n",
                    inet_ntop(s->addr.sin_family, &s->addr.sin_addr,
                            buf, sizeof(buf)),
                    ntohs(s->addr.sin_port));
        }

        free(s);

        return;
    }

    if (aex_ring_put(wc->mod.ring, s)) {
        fprintf(stderr, "WARNIGN: failed to queue mod: queue is full\n");
        return;
    }

    ev_io_stop(el, &s->re);
    ev_async_send(wc->mod.loop, wc->mod.aw);
}

static void
aex_stream_write_event(struct ev_loop * const el, ev_io * const e,
        const int f) {
    aex_stream_t * const s = (void *) ((uint8_t *) e
            - offsetof(aex_stream_t, we));
    ssize_t              len;

    if (s->buf.len == 0) return;

    len = write(s->fd, s->buf.data, s->buf.len);
    if (len < 0) {
        perror("WARNING: failed to write to socket");
        return;
    }
    s->buf.len -= len;
    if (s->buf.len == 0) ev_io_stop(el, &s->we);

    if (s->buf.len < sizeof(s->buf.data)) ev_io_start(el, &s->re);
}

static void
aex_stream_listen_event(struct ev_loop * const el, ev_io * const e,
        const int f) {
    aex_worker_io_t * const wc = (void *) ((uint8_t *) e
            - offsetof(aex_worker_io_t, ae));

    aex_stream_t * const s = calloc(1, sizeof(aex_stream_t));
    if (s == NULL) {
        fprintf(stderr, "WARNING: failed to allocate stream: "
                "not enough memory\n");
        return;
    }

    socklen_t sl = sizeof(s->addr);
    s->fd = accept(wc->sock, (struct sockaddr *) &s->addr, &sl);
    if (s->fd == -1) {
        perror("WARNING: failed to accept new stream");
        free(s);
        return;
    }

    ev_io_init(&s->re, aex_stream_read_event, s->fd, EV_READ);
    ev_io_init(&s->we, aex_stream_write_event, s->fd, EV_WRITE);
    ev_io_start(el, &s->re);

    {
        char buf[INET_ADDRSTRLEN];
        fprintf(stdout, "INFO: new stream: %s:%u:%d\n",
                inet_ntop(s->addr.sin_family, &s->addr.sin_addr,
                        buf, sizeof(buf)),
                ntohs(s->addr.sin_port), s->fd);
    }
}

static void *
aex_worker_mod_handler(void * const data) {
    aex_worker_mod_t * const wc = data;
    struct ev_loop   * const loop = loops[0];

    ev_set_userdata(loop, wc);

    ev_async_init(&wc->aw, aex_mod_event);
    ev_async_start(loop, &wc->aw);

    ev_async_init(&wc->sw, aex_stop_event);
    ev_async_start(loop, &wc->sw);

    ev_run(loop, 0);

    ev_async_stop(loop, &wc->aw);
    ev_async_stop(loop, &wc->sw);

    return NULL;
}

static void *
aex_worker_io_handler(void * const data) {
    aex_worker_io_t * const wc = data;
    struct ev_loop  * const loop = loops[1];

    ev_set_userdata(loop, wc);

    ev_async_init(&wc->aw, aex_io_event);
    ev_async_start(loop, &wc->aw);

    ev_async_init(&wc->sw, aex_stop_event);
    ev_async_start(loop, &wc->sw);

    ev_io_init(&wc->ae, aex_stream_listen_event, wc->sock, EV_READ);
    ev_io_start(loop, &wc->ae);

    ev_run(loop, 0);

    ev_io_stop(loop, &wc->ae);
    ev_async_stop(loop, &wc->aw);
    ev_async_stop(loop, &wc->sw);

    return NULL;
}

static void
aex_signal_handler(int sig) {
    for (uint8_t i = 0; i < 2; i++)
        ev_async_send(loops[i], ev_userdata(loops[i]));
}

int
main(int argc, char **argv) {
    struct sigaction       sa = {0};
    sigset_t               ss = {0};
    aex_worker_mod_t       wmod = {0};
    aex_worker_io_t        wio = { .sock = -1 };
    pthread_t              tio, tmod;
    uint32_t               b = 1024, r = 1024;
    uint16_t               p = htons(1234);
    int                    ch;

    /* options */
    while ((ch	= getopt(argc, argv, "b:p:r:")) != -1) {
        switch (ch) {
            case 'b':
                b = strtoull(optarg, NULL, 10);
                break;

            case 'p':
                p = htons(strtoull(optarg, NULL, 10));
                break;

            case 'r':
                r = strtoull(optarg, NULL, 10);
                break;

            default:
                return EXIT_SUCCESS;
        }
    }
    argc -= optind;
    argv += optind;

    loops[0] = EV_DEFAULT;
    loops[1] = ev_loop_new(0);
    if (loops[1] == NULL) return EXIT_FAILURE;

    /* set signals */
    sigemptyset(&ss);
    sigaddset(&ss, SIGINT);
    sigaddset(&ss, SIGTERM);

    sa.sa_handler = aex_signal_handler;
    sa.sa_mask = ss;
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &sa, NULL)) {
        perror("ERROR: failed to setup signal handler");
        goto error;
    }
    if (sigaction(SIGTERM, &sa, NULL)) {
        perror("ERROR: failed to setup signal handler");
        goto error;
    }

    wio.sock = socket(AF_INET, SOCK_STREAM, 0);
    if (wio.sock == -1) {
        perror("ERROR: failed to create stream listen socket");
        goto error;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = p,
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(wio.sock, (struct sockaddr *) &addr, sizeof(addr))) {
        perror("ERROR: failed to bind stream listen socket");
        goto error;
    }

    if (listen(wio.sock, b)) {
        perror("ERROR: failed to bind stream listen socket");
        goto error;
    }

    {
        aex_ring_t * const ior = aex_ring_create(r);
        if (ior == NULL) goto error;

        aex_ring_t * const modr = aex_ring_create(r);
        if (modr == NULL) goto error;

        wmod.ring = modr;
        wmod.io.aw = &wio.aw;
        wmod.io.ring = ior;
        wmod.io.loop = loops[1];
        if (pthread_create(&tmod, NULL, aex_worker_mod_handler, &wmod)) {
            perror("ERROR: failed to create mod thread");
            goto error;
        }

        wio.ring = ior;
        wio.mod.aw = &wmod.aw;
        wio.mod.ring = modr;
        wio.mod.loop = loops[0];
        if (pthread_create(&tio, NULL, aex_worker_io_handler, &wio)) {
            perror("ERROR: failed to create I/O thread");
            goto error;
        }
    }

    pthread_join(tio, NULL);
    pthread_join(tmod, NULL);

    aex_ring_destroy(wmod.io.ring);
    aex_ring_destroy(wio.mod.ring);
    ev_loop_destroy(loops[1]);

    close(wio.sock);

    return EXIT_SUCCESS;

error:
    aex_ring_destroy(wmod.io.ring);
    aex_ring_destroy(wio.mod.ring);
    ev_loop_destroy(loops[1]);

    close(wio.sock);

    return EXIT_FAILURE;
}

