/* sim_threads.h — minimal persistent thread pool for the particle core (M4).
 *
 * Header-only, static: included by exactly one TU (sim_particles.c). Zero
 * external deps beyond pthreads (always present on POSIX) — keeps the core
 * "zero deps" while mirroring the future compute-shader dispatch model:
 * a parallel_for splits [0,n) into chunks pulled by persistent workers via an
 * atomic cursor (dynamic load balance), blocking until the whole range is done.
 *
 * Determinism: chunk-to-worker assignment does NOT affect results as long as the
 * job writes disjoint data per index (every parallel_for here does); the worker
 * index is passed so jobs can accumulate diagnostics into per-worker slots and
 * the caller reduces them in fixed order.
 */
#ifndef SIM_THREADS_H
#define SIM_THREADS_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>          /* GetSystemInfo: no sysconf on mingw-w64 */
#else
#include <unistd.h>
#endif

typedef void (*SimJob)(void *arg, int worker, int begin, int end);

typedef struct SimPool {
    pthread_t *th;
    int        nth;                 /* >=1; 1 means "no worker threads, run on caller" */
    pthread_mutex_t m;
    pthread_cond_t  cv_work, cv_done;
    SimJob      fn; void *arg;
    int         n, chunk;
    atomic_int  next;               /* next chunk start */
    int         working;            /* workers still in the current job */
    unsigned    seq;                /* job generation */
    int         stop;
} SimPool;

static void simpool_run_chunks(SimPool *p, int worker) {
    int c;
    while ((c = atomic_fetch_add(&p->next, p->chunk)) < p->n) {
        int e = c + p->chunk; if (e > p->n) e = p->n;
        p->fn(p->arg, worker, c, e);
    }
}

static void *simpool_worker(void *vp) {
    /* worker index is encoded by pointer arithmetic against the array base */
    struct { SimPool *p; int idx; } *w = vp;
    SimPool *p = w->p; int idx = w->idx; free(w);
    unsigned myseq = 0;
    for (;;) {
        pthread_mutex_lock(&p->m);
        while (!p->stop && p->seq == myseq) pthread_cond_wait(&p->cv_work, &p->m);
        if (p->stop) { pthread_mutex_unlock(&p->m); return NULL; }
        myseq = p->seq;
        pthread_mutex_unlock(&p->m);

        simpool_run_chunks(p, idx);

        pthread_mutex_lock(&p->m);
        if (--p->working == 0) pthread_cond_signal(&p->cv_done);
        pthread_mutex_unlock(&p->m);
    }
}

static int simpool_default_threads(void) {
    const char *e = getenv("SIMP_THREADS");
    if (e) { int n = atoi(e); return n > 0 ? n : 1; }
#if defined(_WIN32)
    SYSTEM_INFO si; GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? (int)n : 1;
}

static SimPool *simpool_create(int nthreads) {
    if (nthreads < 1) nthreads = 1;
    SimPool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->nth = nthreads;
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->cv_work, NULL);
    pthread_cond_init(&p->cv_done, NULL);
    atomic_init(&p->next, 0);
    p->seq = 0; p->stop = 0;
    if (nthreads > 1) {
        p->th = calloc(nthreads, sizeof(pthread_t));
        /* worker 0 is the caller thread during a job; spawn nthreads-1 workers
         * (indices 1..nthreads-1) so every job sees workers 0..nthreads-1. */
        for (int i = 1; i < nthreads; i++) {
            struct { SimPool *p; int idx; } *w = malloc(sizeof *w);
            w->p = p; w->idx = i;
            pthread_create(&p->th[i], NULL, simpool_worker, w);
        }
    }
    return p;
}

static void simpool_destroy(SimPool *p) {
    if (!p) return;
    if (p->nth > 1) {
        pthread_mutex_lock(&p->m);
        p->stop = 1; p->seq++;
        pthread_cond_broadcast(&p->cv_work);
        pthread_mutex_unlock(&p->m);
        for (int i = 1; i < p->nth; i++) pthread_join(p->th[i], NULL);
        free(p->th);
    }
    pthread_mutex_destroy(&p->m);
    pthread_cond_destroy(&p->cv_work);
    pthread_cond_destroy(&p->cv_done);
    free(p);
}

static inline int simpool_nthreads(const SimPool *p) { return p ? p->nth : 1; }

/* Run fn over [0,n) in chunks, blocking until done. Worker 0 = caller thread;
 * extra work runs on the pool. fn(arg, worker, begin, end) for sub-ranges. */
static void simpool_parallel_for(SimPool *p, int n, int chunk, SimJob fn, void *arg) {
    if (n <= 0) return;
    if (chunk < 1) chunk = 1;
    if (!p || p->nth <= 1) { fn(arg, 0, 0, n); return; }

    pthread_mutex_lock(&p->m);
    p->fn = fn; p->arg = arg; p->n = n; p->chunk = chunk;
    atomic_store(&p->next, 0);
    p->working = p->nth;          /* workers 1..nth-1 + the caller (worker 0) */
    p->seq++;
    pthread_cond_broadcast(&p->cv_work);
    pthread_mutex_unlock(&p->m);

    /* the caller participates as worker 0 */
    simpool_run_chunks(p, 0);

    pthread_mutex_lock(&p->m);
    if (--p->working == 0) pthread_cond_signal(&p->cv_done);
    while (p->working > 0) pthread_cond_wait(&p->cv_done, &p->m);
    pthread_mutex_unlock(&p->m);
}

#endif /* SIM_THREADS_H */
