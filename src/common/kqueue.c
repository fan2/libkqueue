/*
 * Copyright (c) 2009 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>

#include "private.h"

int KQUEUE_DEBUG = 0;


static RB_HEAD(kqt, kqueue) kqtree       = RB_INITIALIZER(&kqtree);
static pthread_rwlock_t     kqtree_mtx;

int CONSTRUCTOR
_libkqueue_init(void)
{
#ifdef NDEBUG
    KQUEUE_DEBUG = 0;
#elif _WIN32
	/* Experimental port, always debug */
	KQUEUE_DEBUG = 1;
#else
    KQUEUE_DEBUG = (getenv("KQUEUE_DEBUG") == NULL) ? 0 : 1;
#endif

   pthread_rwlock_init(&kqtree_mtx, NULL);

   dbg_puts("library initialization complete");
   return (0);
}

static int
kqueue_cmp(struct kqueue *a, struct kqueue *b)
{
    return memcmp(&a->kq_evfd.ef_id, &b->kq_evfd.ef_id, sizeof(int)); 
}

RB_GENERATE(kqt, kqueue, entries, kqueue_cmp)

/* Must hold the kqtree_mtx when calling this */
void
kqueue_free(struct kqueue *kq)
{
    RB_REMOVE(kqt, &kqtree, kq);
    filter_unregister_all(kq);
    kqops.kqueue_free(kq);
    free(kq);
}

void
kqueue_put(struct kqueue *kq)
{
    atomic_dec(&kq->kq_ref);
    /* TODO: free() object when refcount == 0 */
}

struct kqueue *
kqueue_get(int kq)
{
    struct kqueue query;
    struct kqueue *ent = NULL;

    query.kq_evfd.ef_id = kq;
    pthread_rwlock_rdlock(&kqtree_mtx);
    ent = RB_FIND(kqt, &kqtree, &query);
    pthread_rwlock_unlock(&kqtree_mtx);

    if (ent != NULL)
        atomic_inc(&ent->kq_ref);

    return (ent);
}

int VISIBLE
kqueue(void)
{
	struct kqueue *kq;

    kq = calloc(1, sizeof(*kq));
    if (kq == NULL)
        return (-1);
    kq->kq_ref = 1;
    pthread_mutex_init(&kq->kq_mtx, NULL);


    if (kqops.eventfd_init(&kq->kq_evfd) < 0)
        goto errout_unlocked;
    if (kqops.kqueue_init(kq) < 0)
        goto errout_unlocked;

    pthread_rwlock_wrlock(&kqtree_mtx);
    /* TODO: move outside of the lock if it is safe */
    if (filter_register_all(kq) < 0)
        goto errout;
    RB_INSERT(kqt, &kqtree, kq);
    pthread_rwlock_unlock(&kqtree_mtx);

    dbg_printf("created kqueue, fd=%d", kqueue_id(kq));
    return (kqops.eventfd_descriptor(&kq->kq_evfd));

errout:
    pthread_rwlock_unlock(&kqtree_mtx);

errout_unlocked:
    kqops.kqueue_free(kq);
    free(kq);
    return (-1);
}
