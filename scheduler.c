/*
 * scheduler.c  --  queue, policy, worker slots, dispatcher thread.  OWNER: B.
 *
 * One mutex (g_lock) protects the queue, the worker table and the knobs.
 * Rule: g_lock is NEVER held while calling into jobs.c or into the
 * send function, so there is no lock-order problem with D's table lock.
 *
 * The queue is a singly linked list through j->next, kept in "age order"
 * (oldest first).  Every pick scans the whole list: O(n), fine for a
 * project whose queue holds a few hundred jobs, and it keeps the policy
 * code to one small function (score()).
 */
#include "scheduler.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_WORKERS    64
#define DEFAULT_AGING  10

typedef struct { int used, wid, K, free; } worker_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

static job_t         *g_head   = NULL;
static int            g_qlen   = 0;
static worker_t       g_workers[MAX_WORKERS];
static sched_policy_t g_policy = POLICY_PRIORITY;
static long           g_aging  = DEFAULT_AGING;

static FILE  *g_trace = NULL;
static long (*g_now)(void) = NULL;
static int    g_t0_set = 0;
static long   g_t0 = 0;

static pthread_t g_thread;
static int       g_running = 0;
static int       g_stop    = 0;
static int     (*g_send)(int, job_t *) = NULL;

/* ----------------------------------------------------------------- helpers */

static long now(void) { return g_now ? g_now() : (long)time(NULL); }

static const char *policy_name(sched_policy_t p)
{
    switch (p) {
    case POLICY_FIFO:     return "fifo";
    case POLICY_PRIORITY: return "priority";
    case POLICY_SJF:      return "sjf";
    }
    return "?";
}

/* caller holds g_lock */
static void trace(const char *fmt, ...)
{
    if (!g_trace) return;
    long t = now();
    if (!g_t0_set) { g_t0 = t; g_t0_set = 1; }
    fprintf(g_trace, "[t=%3ld] ", t - g_t0);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_trace, fmt, ap);
    va_end(ap);
    fputc('\n', g_trace);
    fflush(g_trace);
}

static long waited(const job_t *j, long t)
{
    long w = t - j->submit_time;
    return w < 0 ? 0 : w;
}

/* "Higher is better" for every policy.  Integer arithmetic only: with
 * aging T the priority score is priority*T + wait, which is T times the
 * effective priority priority + wait/T, so the order is the same without
 * any rounding.  FIFO scores everybody 0, so the strict '>' in take_best()
 * leaves the oldest job (the head) in front. */
static long score(const job_t *j, long t)
{
    long w = waited(j, t);
    switch (g_policy) {
    case POLICY_PRIORITY:
        return g_aging > 0 ? (long)j->priority * g_aging + w : (long)j->priority;
    case POLICY_SJF:
        return g_aging > 0 ? w - (long)j->timeout * g_aging : -(long)j->timeout;
    case POLICY_FIFO:
        break;
    }
    return 0;
}

/* Human-readable effective value for the trace (the number the report shows). */
static void fmt_eff(const job_t *j, long t, char *buf, size_t n)
{
    double w = (double)waited(j, t);
    switch (g_policy) {
    case POLICY_PRIORITY:
        snprintf(buf, n, " eff=%.1f", j->priority + (g_aging > 0 ? w / g_aging : 0.0));
        break;
    case POLICY_SJF:
        snprintf(buf, n, " eff=%.1f", j->timeout - (g_aging > 0 ? w / g_aging : 0.0));
        break;
    default:
        buf[0] = '\0';
    }
}

static int in_queue(const job_t *j)
{
    for (const job_t *p = g_head; p; p = p->next)
        if (p == j) return 1;
    return 0;
}

static worker_t *find_worker(int wid)
{
    for (int i = 0; i < MAX_WORKERS; i++)
        if (g_workers[i].used && g_workers[i].wid == wid) return &g_workers[i];
    return NULL;
}

/* the worker with the most free slots (ties: lowest table index) */
static worker_t *best_worker(void)
{
    worker_t *b = NULL;
    for (int i = 0; i < MAX_WORKERS; i++)
        if (g_workers[i].used && g_workers[i].free > 0 &&
            (!b || g_workers[i].free > b->free))
            b = &g_workers[i];
    return b;
}

/* Take the best job out of the queue.  Caller holds g_lock. */
static job_t *take_best(long t)
{
    if (!g_head) return NULL;
    job_t **best = &g_head;
    long bs = score(g_head, t);
    for (job_t **pp = &g_head->next; *pp; pp = &(*pp)->next) {
        long s = score(*pp, t);
        if (s > bs) { best = pp; bs = s; }       /* strict: ties keep the older job */
    }
    job_t *j = *best;
    *best = j->next;
    j->next = NULL;
    g_qlen--;
    return j;
}

static void trace_pick(const char *what, const job_t *j, long t, int wid)
{
    if (!g_trace) return;
    char eff[40];
    fmt_eff(j, t, eff, sizeof eff);
    if (wid >= 0)
        trace("%-8s %s policy=%s prio=%d timeout=%d wait=%lds%s -> worker %d qlen=%d",
              what, j->id, policy_name(g_policy), j->priority, j->timeout,
              waited(j, t), eff, wid, g_qlen);
    else
        trace("%-8s %s policy=%s prio=%d timeout=%d wait=%lds%s qlen=%d",
              what, j->id, policy_name(g_policy), j->priority, j->timeout,
              waited(j, t), eff, g_qlen);
}

/* ---------------------------------------------------------------- life cycle */

void sched_init(void)
{
    pthread_mutex_lock(&g_lock);
    g_head = NULL;
    g_qlen = 0;
    memset(g_workers, 0, sizeof g_workers);
    g_policy = POLICY_PRIORITY;
    g_aging  = DEFAULT_AGING;
    g_trace  = NULL;
    g_now    = NULL;
    g_t0_set = 0;
    g_send   = NULL;

    const char *e = getenv("SCHED_POLICY");
    if (e) {
        if (!strcasecmp(e, "fifo"))          g_policy = POLICY_FIFO;
        else if (!strcasecmp(e, "priority")) g_policy = POLICY_PRIORITY;
        else if (!strcasecmp(e, "sjf"))      g_policy = POLICY_SJF;
    }
    e = getenv("SCHED_AGING");
    if (e) {
        long T = atol(e);
        g_aging = T < 0 ? 0 : T;
    }
    pthread_mutex_unlock(&g_lock);
}

/* ----------------------------------------------------------------- the queue */

void sched_add(job_t *j)
{
    pthread_mutex_lock(&g_lock);
    if (!in_queue(j)) {
        job_t **pp = &g_head;
        while (*pp) pp = &(*pp)->next;
        j->next = NULL;
        *pp = j;
        g_qlen++;
        trace("ADD      %s prio=%d timeout=%d qlen=%d", j->id, j->priority, j->timeout, g_qlen);
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_lock);
}

/* Keeps the list in submit_time order, so a re-queued job goes in front of
 * newer jobs and keeps its age (its original submit_time is untouched). */
void sched_requeue(job_t *j)
{
    pthread_mutex_lock(&g_lock);
    if (!in_queue(j)) {
        job_t **pp = &g_head;
        while (*pp && (*pp)->submit_time <= j->submit_time) pp = &(*pp)->next;
        j->next = *pp;
        *pp = j;
        g_qlen++;
        trace("REQUEUE  %s prio=%d timeout=%d qlen=%d", j->id, j->priority, j->timeout, g_qlen);
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_lock);
}

int sched_remove(const char *id)
{
    int found = 0;
    if (!id) return 0;
    pthread_mutex_lock(&g_lock);
    for (job_t **pp = &g_head; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->id, id) == 0) {
            job_t *j = *pp;
            *pp = j->next;
            j->next = NULL;
            g_qlen--;
            trace("REMOVE   %s (left the queue) qlen=%d", j->id, g_qlen);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

int sched_queue_length(void)
{
    pthread_mutex_lock(&g_lock);
    int n = g_qlen;
    pthread_mutex_unlock(&g_lock);
    return n;
}

job_t *sched_next(long t)
{
    pthread_mutex_lock(&g_lock);
    job_t *j = take_best(t);
    if (j) trace_pick("PICK", j, t, -1);
    pthread_mutex_unlock(&g_lock);
    return j;
}

/* ------------------------------------------------------------------- workers */

int sched_worker_add(int wid, int K)
{
    int rc = -1;
    if (wid < 0 || K < 1) return -1;
    pthread_mutex_lock(&g_lock);
    if (!find_worker(wid)) {
        for (int i = 0; i < MAX_WORKERS; i++) {
            if (!g_workers[i].used) {
                g_workers[i].used = 1;
                g_workers[i].wid  = wid;
                g_workers[i].K    = K;
                g_workers[i].free = K;
                trace("WORKER+  %d K=%d", wid, K);
                pthread_cond_broadcast(&g_cv);
                rc = 0;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void sched_worker_remove(int wid)
{
    pthread_mutex_lock(&g_lock);
    worker_t *w = find_worker(wid);
    if (w) {
        w->used = 0;
        trace("WORKER-  %d", wid);
    }
    pthread_mutex_unlock(&g_lock);
}

void sched_slot_release(int wid)
{
    pthread_mutex_lock(&g_lock);
    worker_t *w = find_worker(wid);
    if (w) {
        if (w->free < w->K) w->free++;            /* never above K */
        trace("SLOT+    worker %d free=%d/%d", wid, w->free, w->K);
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_lock);
}

int sched_free_slots(int wid)
{
    pthread_mutex_lock(&g_lock);
    worker_t *w = find_worker(wid);
    int n = w ? w->free : -1;
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* --------------------------------------------------------------------- knobs */

void sched_set_policy(sched_policy_t p)
{
    pthread_mutex_lock(&g_lock);
    g_policy = p;
    trace("POLICY   now %s (aging T=%lds)", policy_name(p), g_aging);
    pthread_mutex_unlock(&g_lock);
}

sched_policy_t sched_get_policy(void)
{
    pthread_mutex_lock(&g_lock);
    sched_policy_t p = g_policy;
    pthread_mutex_unlock(&g_lock);
    return p;
}

void sched_set_aging(long T)
{
    pthread_mutex_lock(&g_lock);
    g_aging = T < 0 ? 0 : T;
    trace("AGING    T=%lds", g_aging);
    pthread_mutex_unlock(&g_lock);
}

long sched_get_aging(void)
{
    pthread_mutex_lock(&g_lock);
    long T = g_aging;
    pthread_mutex_unlock(&g_lock);
    return T;
}

void sched_set_trace(FILE *f)
{
    pthread_mutex_lock(&g_lock);
    g_trace = f;
    pthread_mutex_unlock(&g_lock);
}

void sched_set_clock(long (*now_fn)(void))
{
    pthread_mutex_lock(&g_lock);
    g_now = now_fn;
    pthread_mutex_unlock(&g_lock);
}

/* ---------------------------------------------------------------- dispatcher */

/* Steps b..d of the contract in scheduler.h.  Runs WITHOUT g_lock. */
static void run_dispatch(job_t *j, int wid, int (*send)(int, job_t *))
{
    job_set_worker(j, wid);

    if (!job_set_state(j, J_RUNNING, 0)) {         /* cancelled before we got here */
        job_set_worker(j, -1);
        pthread_mutex_lock(&g_lock);
        trace("SKIP     %s was cancelled before dispatch", j->id);
        pthread_mutex_unlock(&g_lock);
        sched_slot_release(wid);
        return;
    }

    if (send(wid, j) != 0) {                       /* worker connection is dead */
        sched_worker_remove(wid);
        job_set_worker(j, -1);
        if (job_set_state(j, J_QUEUED, 0))
            sched_requeue(j);
        return;                                    /* cancelled meanwhile: terminal, nothing to do */
    }

    /* A cancel that arrived between RUNNING and the send wrote its kill
     * message BEFORE the job message.  Send one more, now it is ordered
     * after the job. */
    if (job_get_state(j, NULL) == J_CANCELLED)
        job_send_kill(j);
}

static void *dispatcher_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    while (!g_stop) {
        worker_t *w = best_worker();
        if (!w || !g_head) {
            pthread_cond_wait(&g_cv, &g_lock);
            continue;
        }
        long t = now();
        job_t *j = take_best(t);
        int wid = w->wid;
        w->free--;
        trace_pick("DISPATCH", j, t, wid);
        int (*send)(int, job_t *) = g_send;

        pthread_mutex_unlock(&g_lock);
        run_dispatch(j, wid, send);
        pthread_mutex_lock(&g_lock);
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int sched_dispatcher_start(int (*send_job)(int wid, job_t *j))
{
    if (!send_job) return -1;
    pthread_mutex_lock(&g_lock);
    if (g_running) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    g_send = send_job;
    g_stop = 0;
    g_running = 1;
    pthread_mutex_unlock(&g_lock);

    if (pthread_create(&g_thread, NULL, dispatcher_main, NULL) != 0) {
        pthread_mutex_lock(&g_lock);
        g_running = 0;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    return 0;
}

void sched_dispatcher_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_stop = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);

    pthread_join(g_thread, NULL);

    pthread_mutex_lock(&g_lock);
    g_running = 0;
    pthread_mutex_unlock(&g_lock);
}
