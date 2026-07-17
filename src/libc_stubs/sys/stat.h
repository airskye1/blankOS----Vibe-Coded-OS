#ifndef SYS_STAT_H
#define SYS_STAT_H

#include <sys/types.h>

struct stat {
    off_t st_size;
    mode_t st_mode;
};

#define S_ISDIR(m) (((m) & 0170000) == 0040000)

#ifdef __cplusplus
extern "C" {
#endif

int stat(const char* pathname, struct stat* statbuf);
int mkdir(const char* pathname, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
