#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void free(void* ptr);
void* realloc(void* ptr, size_t size);
void* calloc(size_t nmemb, size_t size);
void exit(int status);
char* getenv(const char* name);
int atoi(const char* nptr);
double atof(const char* nptr);
long strtol(const char* nptr, char** endptr, int base);
int abs(int j);
void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));

#ifdef __cplusplus
}
#endif

#endif
