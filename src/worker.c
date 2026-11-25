#include "job_runner.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

int main(void)
{
    Job job = {
        .job_id = "TEST-1",
        .pid = -1,
        .timeout = 10,
        .output_fd = -1
    };

    if (start_job(&job,
        "echo Hello from stdout; echo Hello from stderr >&2") == -1) {
        return 1;
    }

    printf("Started job %s, PID: %d\n",
           job.job_id, (int)job.pid);

    char buffer[1024];
    ssize_t bytes;

    printf("Captured output:\n");

    while (1) {
        bytes = read(job.output_fd, buffer, sizeof(buffer));

        if (bytes > 0) {
            fwrite(buffer, 1, bytes, stdout);
        } else if (bytes == 0) {
            break;
        } else if (errno != EINTR) {
            perror("read");
            break;
        }
    }

    close(job.output_fd);

    waitpid(job.pid, NULL, 0);

    printf("Job finished\n");

    return 0;
}