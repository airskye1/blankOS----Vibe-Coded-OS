#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FILE FILE;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

FILE* fopen(const char* filename, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
int fprintf(FILE* stream, const char* format, ...);
int printf(const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int sscanf(const char* str, const char* format, ...);
int vsnprintf(char* str, size_t size, const char* format, va_list ap);
int rename(const char* oldname, const char* newname);
int remove(const char* filename);
void perror(const char* s);
int fflush(FILE* stream);

#ifdef __cplusplus
}
#endif

#endif
