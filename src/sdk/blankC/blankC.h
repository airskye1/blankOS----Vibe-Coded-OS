#ifndef BLANKC_H
#define BLANKC_H

// blankC: The BlankOS Distribution of the Standard C Library
// This header is included by 3rd party developers writing .bloe applications

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Core Memory (Wrappers around Kernel Syscalls)
extern void* blank_malloc(size_t size);
extern void blank_free(void* ptr);

// String Operations
extern int blank_strlen(const char* str);
extern void blank_strcpy(char* dest, const char* src);
extern int blank_strcmp(const char* s1, const char* s2);

// blankUI Bindings (Allows developers to easily access the native UI framework)
extern void bUI_window(int width, int height, char* title);
extern void bUI_button(int x, int y, int w, int h, char* label, void (*onclick)(void));
extern void bUI_text(int x, int y, char* text);
extern void bUI_toast(char* title, char* msg);

#endif // BLANKC_H
