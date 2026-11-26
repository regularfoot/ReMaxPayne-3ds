#ifndef MP_NATIVE_COMIC_H
#define MP_NATIVE_COMIC_H
#include <stdbool.h>
#include <stdio.h>

bool comic_read(unsigned first,unsigned last,FILE *journal);
bool comic_campaign(const char *const *names,unsigned count,FILE *journal);
#endif
