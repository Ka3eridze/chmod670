#ifndef JOB_RUNNER_H
#define JOB_RUNNER_H

#include <sys/types.h>
#include <time.h>

typedef enum {
    JOB_RUNNING,
    JOB_DONE,
    JOB_KILLED,
    JOB_TIMEOUT,
    JOB_CANCELLED
} JobState;

typedef struct {
    char job_id[64];
    pid_t pid;
    int timeout;
    int output_fd;

    JobState state;
    int exit_code;
    int term_signal;

    struct timespec start_time;
} Job;

int start_job(Job *job, const char *command);
int check_job(Job *job);
int check_timeout(Job *job);

int cancel_job(Job *job);

#endif