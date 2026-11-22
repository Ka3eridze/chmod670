
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <sys/types.h>

#define PROTO_MAX_LINE 4096 //Max length of a line in the protocol

ssize_t proto_send_all(int fd, const void *buffer, size_t length); // sends all data in buffer

ssize_t proto_read_line(int fd, char *buffer, size_t capacity); //reads a line from the file desc

int proto_read_exact(int fd, void *buffer, size_t length); // reads  exactly length bytes from the file

#endif
