#include "job_runner.h"
#include <unistd.h>
#include <stdio.h>

int start_job(Job *job, const char *command)
{
    int pipefd[2];

    if (pipe(pipefd) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1 ||
            dup2(pipefd[1], STDERR_FILENO) == -1) {
            perror("dup2");
            _exit(127);
        }

        close(pipefd[1]);

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);

        perror("exec");
        _exit(127);
    }

    close(pipefd[1]);

    job->pid = pid;
    job->output_fd = pipefd[0];

    return 0;
}