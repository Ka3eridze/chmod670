#ifndef JOB_RUNNER_H
#define JOB_RUNNER_H

#include <sys/types.h>

typedef struct {
    char job_id[64];
    pid_t pid;
    int timeout;
    int output_fd;
} Job;

int start_job(Job *job, const char *command);

#endif