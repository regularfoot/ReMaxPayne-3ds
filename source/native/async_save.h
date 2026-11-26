#ifndef MP_ASYNC_SAVE_H
#define MP_ASYNC_SAVE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned slot;uint32_t signature;
    char stage[32],room[64];
    void *data;size_t size;bool ok;
} SaveJob;

// Start under the loading screen. Submission takes ownership only on success.
bool async_save_open(void);
bool async_save_submit(const SaveJob *job);
bool async_save_result(SaveJob *result);
// Only call while loading, paused, or leaving the game loop.
void async_save_finish(void);
void async_save_close(void);
#endif
