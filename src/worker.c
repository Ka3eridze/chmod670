#include "job_runner.h"
#include <stdio.h>
#include <sys/wait.h>

int main(void)
{
    Job job = {
        .job_id = "TEST-1",
        .pid = -1,
        .timeout = 10
    };

    if (start_job(&job, "echo Hello from Worker") == -1) {
        return 1;
    }

    printf("Started job %s, PID: %d\n",
           job.job_id, (int)job.pid);

    waitpid(job.pid, NULL, 0);

    printf("Job finished\n");

    return 0;
}