
#include "protocol.h"

#include <errno.h>
#include <sys/socket.h>

// Send all bytes
ssize_t proto_send_all(int fd, const void *buffer, size_t length) {
    const char *data = buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, data + sent,
                         length - sent, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            if (n == 0)
                errno = EPIPE;
            return -1;
        }

        sent += (size_t)n;
    }

    return (ssize_t)sent;
}

// Read one line ending with '\n'
ssize_t proto_read_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;

    if (capacity < 2) {
        errno = EINVAL;
        return -1;
    }

    while (1) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            if (n == 0)
                errno = ECONNRESET;
            return -1;
        }

        if (ch == '\n') {
            buffer[used] = '\0';
            return (ssize_t)used;
        }

        if (ch == '\0') {
            errno = EPROTO;
            return -1;
        }

        if (used >= capacity - 1) {
            errno = EMSGSIZE;
            return -1;
        }

        buffer[used++] = ch;
    }
}

// Read exactly length bytes
int proto_read_exact(int fd, void *buffer, size_t length) {
    char *data = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t n = recv(fd, data + received,
                         length - received, 0);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            if (n == 0)
                errno = ECONNRESET;
            return -1;
        }

        received += (size_t)n;
    }

    return 0;
}
