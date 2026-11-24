#include "job_runner.h"
#include <unistd.h>
#include <stdio.h>

int start_job(Job *job, const char *command)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);

        perror("exec");
        _exit(127);
    }

    job->pid = pid;

    return 0;
}