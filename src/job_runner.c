#define _POSIX_C_SOURCE 200809L

#include "job_runner.h"

#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

int start_job(Job *job, const char *command)
{
    int pipefd[2];

    if (pipe(pipefd) == -1) {
        perror("pipe");
        return -1;
    }

    int flags = fcntl(pipefd[0], F_GETFL);

    if (flags == -1 ||
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &job->start_time) == -1) {
        perror("clock_gettime");
        close(pipefd[0]);
        close(pipefd[1]);
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
        if (setpgid(0, 0) == -1) {
            perror("setpgid");
            _exit(127);
        }

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

    // Establish the process group from the parent side too.
    if (setpgid(pid, pid) == -1 &&
        errno != EACCES && errno != ESRCH) {
        perror("setpgid");
        kill(pid, SIGKILL);

        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
        }

        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    close(pipefd[1]);

    job->pid = pid;
    job->output_fd = pipefd[0];
    job->state = JOB_RUNNING;
    job->exit_code = -1;
    job->term_signal = 0;

    return 0;
}

int check_job(Job *job)
{
    int status;
    pid_t result;

    do {
        result = waitpid(job->pid, &status, WNOHANG);
    } while (result == -1 && errno == EINTR);

    if (result == 0) {
        return 0;
    }

    if (result == -1) {
        perror("waitpid");
        return -1;
    }

    if (WIFEXITED(status)) {
        job->exit_code = WEXITSTATUS(status);

        if (job->state != JOB_TIMEOUT) {
            job->state = JOB_DONE;
        }
    }
    else if (WIFSIGNALED(status)) {
        job->term_signal = WTERMSIG(status);

        if (job->state != JOB_TIMEOUT) {
            job->state = JOB_KILLED;
        }
    }

    return 1;
}

int check_timeout(Job *job)
{
    if (job->state != JOB_RUNNING) {
        return 0;
    }

    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        perror("clock_gettime");
        return -1;
    }

    double elapsed =
        (now.tv_sec - job->start_time.tv_sec) +
        (now.tv_nsec - job->start_time.tv_nsec) / 1e9;

    if (elapsed >= job->timeout) {
        if (kill(-job->pid, SIGKILL) == -1) {
            if (errno == ESRCH) {
                return 0;
            }

            perror("kill");
            return -1;
        }

        job->state = JOB_TIMEOUT;

        return 1;
    }

    return 0;
}