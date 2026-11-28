
#define _POSIX_C_SOURCE 200809L

#include "job_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <limits.h>
#include <sys/wait.h>

static volatile sig_atomic_t cancel_requested = 0;

static void handle_cancel(int sig)
{
    (void)sig;
    cancel_requested = 1;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <timeout> \"command\"\n",
                argv[0]);
        return 1;
    }

    char *end;
    errno = 0;
    long timeout = strtol(argv[1], &end, 10);

    if (errno != 0 || end == argv[1] ||
        *end != '\0' || timeout <= 0 || timeout > INT_MAX) {
        fprintf(stderr, "Invalid timeout\n");
        return 1;
    }

    Job job = {
        .job_id = "TEST-1",
        .pid = -1,
        .timeout = (int)timeout,
        .output_fd = -1
    };

    // Register cancellation signal handler.
    struct sigaction sa = {0};

    sa.sa_handler = handle_cancel;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    if (start_job(&job, argv[2]) == -1) {
        return 1;
    }

    printf("Started job %s, PID: %d\n",
           job.job_id, (int)job.pid);

    printf("Captured output:\n");

    char buffer[1024];
    int finished = 0;

    while (!finished) {

        // Check for cancellation request.
        if (cancel_requested) {
            cancel_requested = 0;

            if (cancel_job(&job) == -1) {
                goto fail;
            }
        }

        struct pollfd pfd = {
            .fd = job.output_fd,
            .events = POLLIN
        };

        int ready = poll(&pfd, 1, 100);

        if (ready == -1 && errno != EINTR) {
            perror("poll");
            goto fail;
        }

        if (ready > 0 &&
            (pfd.revents & (POLLIN | POLLHUP))) {

            ssize_t bytes;

            while ((bytes = read(job.output_fd,
                                 buffer,
                                 sizeof(buffer))) > 0) {

                fwrite(buffer, 1, bytes, stdout);

                // Check cancellation during continuous output.
                if (cancel_requested) {
                    cancel_requested = 0;

                    if (cancel_job(&job) == -1) {
                        goto fail;
                    }
                }

                // Check timeout during continuous output.
                if (check_timeout(&job) == -1) {
                    goto fail;
                }
            }

            if (bytes == 0) {
                close(job.output_fd);
                job.output_fd = -1;
            }
            else if (errno != EAGAIN &&
                     errno != EWOULDBLOCK &&
                     errno != EINTR) {
                perror("read");
                goto fail;
            }
        }

        int result = check_job(&job);

        if (result == -1) {
            goto fail;
        }

        if (result == 1) {
            finished = 1;
        }
        else if (check_timeout(&job) == -1) {
            goto fail;
        }
    }

    // Read remaining available output.
    if (job.output_fd != -1) {
        ssize_t bytes;

        while ((bytes = read(job.output_fd,
                             buffer,
                             sizeof(buffer))) > 0) {
            fwrite(buffer, 1, bytes, stdout);
        }

        close(job.output_fd);
        job.output_fd = -1;
    }

    // Print final job status.
    switch (job.state) {
        case JOB_DONE:
            printf("DONE %d\n", job.exit_code);
            break;

        case JOB_KILLED:
            printf("KILLED SIGNAL %d\n", job.term_signal);
            break;

        case JOB_TIMEOUT:
            printf("KILLED TIMEOUT\n");
            break;

        case JOB_CANCELLED:
            printf("CANCELLED\n");
            break;

        default:
            break;
    }

    return 0;

fail:
    kill(-job.pid, SIGKILL);

    while (waitpid(job.pid, NULL, 0) == -1 && errno == EINTR) {
    }

    if (job.output_fd != -1) {
        close(job.output_fd);
    }

    return 1;
}