#ifndef MP_SAVE_H
#define MP_SAVE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
enum { SAVE_COUNT=13,SAVE_TRAINING=13,SAVE_BONUS=14,SAVE_DATA_LIMIT=384*1024 };
typedef struct {
    char tag[4],stage[32],room[48];
    uint32_t generation,signature,bytes,crc;
} SaveHeader;
uint32_t save_hash(const void *data,size_t size);
bool save_race_time(unsigned seconds,unsigned *best);
unsigned save_unlocked_modes(void);
bool save_unlock_mode(unsigned mode);
bool save_completed(void);
bool save_completion(void);
bool save_info(unsigned slot,SaveHeader *header);
bool save_loadable(unsigned slot);
unsigned save_autosave_slot(const char *stage);
// Pass the current mode's autosave slot. A negative result means no intact save.
int save_latest(unsigned automatic_slot);
void *save_read(unsigned slot,SaveHeader *header);
bool save_write(unsigned slot,const char *stage,const char *room,uint32_t signature,const void *data,size_t size);
#endif
