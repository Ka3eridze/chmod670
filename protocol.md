# Wire Protocol

## 1. Communication

The client and server communicate using TCP sockets.

Each normal message ends with `\n`. Since TCP is a byte stream, we read in a loop until the complete message is received.

A client sends one request, receives one response, and closes the connection.

For `FETCH`, the server sends the output length first, followed by the exact output bytes.

## 2. Client Requests

The client supports four requests:

- `SUBMIT <timeout> <priority> <command>\n`
- `STATUS <id>\n`
- `CANCEL <id>\n`
- `FETCH <id>\n`

The timeout is a positive integer in seconds. Priority ranges from 0 to 9, with 0 as the default.

The command is the remaining text after timeout and priority, including its spaces and quotes.

Example:

`SUBMIT 10 0 echo hello\n`

This submits the command `echo hello` with a timeout of 10 seconds and priority 0.

## 3. Server Responses

The server uses the following responses:

- `OK <id>\n` — job submitted successfully.
- `STATE <id> <state>\n` — current job status.
- `CANCELLED <id>\n` — job cancelled.
- `ALREADY_FINISHED <id>\n` — job has already finished.
- `DATA <length>\n` — followed by exactly `length` output bytes.
- `ERR UNKNOWN <id>\n` — job ID does not exist.
- `ERR NOT_FINISHED <id>\n` — output is not available yet.
- `ERR BAD_REQUEST\n` — invalid request.

Possible job states are:

- `QUEUED`
- `RUNNING`
- `DONE <exit_code>`
- `KILLED TIMEOUT`
- `KILLED SIGNAL <signal_number>`
- `CANCELLED`

Example:

`STATE T12-1 DONE 0\n`

For `FETCH`, the client reads exactly the number of bytes specified in `DATA`. No extra newline is added to the output.

## 4. Client Output and Errors

The client must follow the command-line format from Term Project Details 1.

- `submit` prints only the new job ID.
- `status` prints the job ID and its current state.
- `cancel` prints `<id> CANCELLED` or `<id> ALREADY FINISHED`.
- `fetch` prints the raw output without adding any text.
- An unknown ID prints `<id> UNKNOWN` and exits with code 2.
- Fetching an unfinished job prints `<id> NOT FINISHED` to stderr and exits with code 1.
- A connection failure prints `client: cannot connect to <host>:<port>` to stderr and exits with code 3.

## 5. Errors and Recovery

If the connection closes before a complete message is received, the incomplete message is rejected.

The server closes client connections after handling their requests.

The server must save accepted jobs in `jobs.log` before returning their IDs to clients.

After a restart, completed jobs keep their results and output, while previously running jobs return to the queue.

## The Server  between Worker message format will be added after discussion with the worker owner.