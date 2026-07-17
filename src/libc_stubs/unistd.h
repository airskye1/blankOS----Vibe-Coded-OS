#ifndef UNISTD_H
#define UNISTD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int access(const char* pathname, int mode);
int read(int fd, void* buf, size_t count);
int write(int fd, const void* buf, size_t count);
int close(int fd);
long lseek(int fd, long offset, int whence);
int unlink(const char* pathname);

#ifdef __cplusplus
}
#endif

#endif
