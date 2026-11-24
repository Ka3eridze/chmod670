/*
 * scheduler.h  --  job queue, scheduling policy, free-slot counts, dispatcher.
 * OWNER: B.   Used by A (server connection code) and D (job table).
 *
 * Everything here is thread-safe (one internal mutex, never held while
 * calling into jobs.c or into A's send function).  The server must be ONE
 * process with threads, because the queue lives in memory.
 *
 * ---------------------------------------------------------------------------
 * WHAT A MUST DO (server side)                         [A = Protocol/client]
 *   1. At startup:  signal(SIGPIPE, SIG_IGN);       (a dead worker must not
 *                                                     kill the server)
 *                   sched_init();
 *                   sched_dispatcher_start(my_send_job);
 *      my_send_job(wid, job) writes the job to worker `wid` over its TCP
 *      connection and returns 0, or -1 if the write failed.  It is called by
 *      the dispatcher thread WITHOUT any scheduler lock held.
 *      The kill hook (job.h) writes to the SAME socket from another thread:
 *      A must serialise writes per worker with a mutex, and should set
 *      SO_SNDTIMEO so a stuck worker cannot block the dispatcher forever.
 *   2. Submit:      j = job_create(cmd, timeout, priority, client_id);
 *                   sched_add(j);       reply with j->id
 *   3. Worker connects and sends K:   sched_worker_add(wid, K);
 *      wid = a small integer, unique per connection and NOT reused (use an
 *      increasing counter), so a late sched_slot_release for a dead
 *      connection can never hit a newer worker.
 *   4. Worker sends a report:   job_set_state(j, DONE/KILLED_*, detail);
 *                               sched_slot_release(wid);
 *      ALWAYS call sched_slot_release(), even if job_set_state() returned 0
 *      (e.g. the job was cancelled): the worker's slot is free either way.
 *   5. Worker connection closes:   sched_worker_remove(wid);   (then see D)
 *   6. On SIGTERM: the handler only sets a volatile sig_atomic_t flag; the
 *      main thread then calls sched_dispatcher_stop().  (Never call it, or
 *      anything with a mutex, inside a signal handler.)
 *
 * WHAT D MUST DO                                  [D = Durability/recovery]
 *   - job_cancel() calls sched_remove() for QUEUED jobs (already written).
 *   - After a worker disconnects, a job D wants to run again:
 *         job_set_worker(j, -1);
 *         if (job_set_state(j, J_QUEUED, 0)) sched_requeue(j);
 *     job_set_state(RUNNING -> QUEUED) must stay allowed.
 *
 * WHAT THE DISPATCHER DOES (B), for each pick  -- the contract the others
 * rely on:
 *   a. under the lock: choose the worker with the most free slots, choose
 *      the job (policy), take the job out of the queue, free_slots--.
 *   b. without the lock: job_set_worker(j, wid); job_set_state(j, RUNNING).
 *      If that is REFUSED (the job was cancelled meanwhile) the job is NOT
 *      sent and the slot is given back.
 *   c. send_job(wid, j).  If it returns -1 the worker is considered dead:
 *      sched_worker_remove(wid), the job goes back to QUEUED and is
 *      re-queued with its original submit time (it keeps its age).
 *   d. after a successful send, if the job has been CANCELLED in the
 *      meantime, the dispatcher calls job_send_kill() once more, so the kill
 *      message is guaranteed to arrive AFTER the job message.  C's worker
 *      must therefore ignore a "kill <unknown id>" quietly.
 *
 * POLICIES (all ties are broken by queue order = oldest first)
 *   FIFO      oldest job first; priority and timeout are ignored.
 *   PRIORITY  highest priority first, with aging:
 *                 effective = priority + waited_seconds / T
 *             uncapped, so a job that waits long enough beats ANY newer job
 *             and nobody starves.  T = sched_set_aging(), default 10 s,
 *             0 = no aging (low priorities CAN starve).
 *   SJF       smallest timeout first, with aging:
 *                 effective = timeout - waited_seconds / T
 *   Default at startup: PRIORITY with T = 10.  With all priorities 0 (the
 *   default for a submit that says nothing) this behaves exactly like FIFO.
 *   Environment, read by sched_init():  SCHED_POLICY=fifo|priority|sjf
 *                                       SCHED_AGING=<seconds>
 *   so the policy can be chosen at the fair without changing the command
 *   line contract.
 * ---------------------------------------------------------------------------
 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdio.h>
#include "jobs.h"

typedef enum { POLICY_FIFO = 0, POLICY_PRIORITY = 1, POLICY_SJF = 2 } sched_policy_t;

/* ---- life cycle ----------------------------------------------------------- */
void sched_init(void);               /* resets everything; dispatcher must be stopped */
int  sched_dispatcher_start(int (*send_job)(int wid, job_t *j));   /* 0 = ok */
void sched_dispatcher_stop(void);    /* returns after the thread has exited */

/* ---- the queue ------------------------------------------------------------ */
void   sched_add(job_t *j);                 /* new QUEUED job (ignored if already queued) */
void   sched_requeue(job_t *j);             /* a job that must run again; keeps its age   */
int    sched_remove(const char *id);        /* 1 = it was queued and is removed, 0 = not in the queue */
int    sched_queue_length(void);

/* The policy decision alone: removes and returns the best job for time `now`,
 * or NULL if the queue is empty.  Ignores worker slots.  The dispatcher uses
 * the same code; it is exposed so that the policy can be tested and traced
 * with a simulated clock. */
job_t *sched_next(long now);

/* ---- workers and their slots --------------------------------------------- */
int  sched_worker_add(int wid, int K);      /* 0 = ok, -1 = wid in use, K < 1, or table full */
void sched_worker_remove(int wid);          /* harmless for an unknown wid */
void sched_slot_release(int wid);           /* a job on this worker has finished; harmless for an unknown wid */
int  sched_free_slots(int wid);             /* free slots, or -1 if unknown      */

/* ---- policy knobs (change them live at the fair) --------------------------- */
void sched_set_policy(sched_policy_t p);
sched_policy_t sched_get_policy(void);
void sched_set_aging(long T);               /* seconds per priority level, 0 = no aging */
long sched_get_aging(void);

/* ---- tracing and testing --------------------------------------------------- */
/* One line per add / pick / dispatch / remove / requeue, flushed at once.
 * Times are seconds since the first traced event.  NULL = off. */
void sched_set_trace(FILE *f);
void sched_set_clock(long (*now_fn)(void)); /* NULL = real time */

#endif
