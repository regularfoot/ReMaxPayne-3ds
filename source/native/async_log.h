#ifndef MP_ASYNC_LOG_H
#define MP_ASYNC_LOG_H
#include <stdio.h>
// A bounded RAM journal. The returned FILE never performs filesystem I/O.
FILE *async_log_open(const char *path);
// Static phase name, or NULL outside an active frame. The logger worker can
// report a stalled phase even when the game thread cannot finish its frame.
void async_log_watch(const char *phase);
void async_log_gpu_frame(unsigned room,unsigned words,unsigned eyes);
// Called from the GSP event thread. 0 is a command list, 1 is a display transfer.
void async_log_gpu_event(unsigned event);
#endif
