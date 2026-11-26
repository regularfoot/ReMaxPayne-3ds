#ifndef MP_LOCALIZATION_H
#define MP_LOCALIZATION_H
#include <stdbool.h>
#include <stddef.h>
bool localization_open(void);
void localization_close(void);
void localization_read(const char *key,char *out,size_t capacity);
#endif
