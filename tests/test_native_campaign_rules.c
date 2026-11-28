#include "campaign_file.h"
#include "game_rules.h"
#include "save.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <utime.h>

FILE *campaign_file(const char *name) {
    assert(!strcmp(name, "difficulty.bin"));
    return fopen(name, "rb");
}

static void damage_checkpoint(unsigned slot,unsigned generation) {
    for (unsigned i = 0; i < 2; i++) {
        char path[64];
        snprintf(path,sizeof path,"sdmc:/3ds/maxpayne/save%02u%c.dat",slot,'a'+i);
        FILE *f = fopen(path, "rb+");
        SaveHeader h;
        if (!f) continue;
        if (fread(&h, sizeof h, 1, f) == 1 && h.generation == generation) {
            assert(fseek(f, sizeof h, SEEK_SET) == 0 && fputc('!', f) != EOF);
            assert(fclose(f) == 0);
            return;
        }
        fclose(f);
    }
    assert(0 && "Checkpoint generation missing");
}

static void verify_checkpoint_recovery(void) {
    const char stage[] = "part0_level1";
    const char *payloads[] = {"initial room", "next room", "retry room"};
    SaveHeader header;
    unsigned initial_generation=0;
    assert(!save_loadable(5));
    for (unsigned i = 0; i < 3; i++) {
        size_t size = strlen(payloads[i]) + 1;
        assert(save_write(5, stage, "test", 42, payloads[i], size));
        assert(save_loadable(5));
        char *written = save_read(5, &header);
        assert(written && header.bytes == size);
        assert(!memcmp(written, payloads[i], size));
        free(written);
        if (i == 0) {initial_generation=header.generation;continue;}
        assert((int32_t)(header.generation-initial_generation)>0);
        damage_checkpoint(5,header.generation);
        assert(save_loadable(5));
        char *fallback = save_read(5, &header);
        assert(fallback && header.generation == initial_generation && !strcmp(fallback, payloads[0]));
        free(fallback);
    }
    // Both banks still have plausible headers, but the load menu must reject them.
    damage_checkpoint(5,initial_generation);
    assert(save_info(5,&header) && !save_loadable(5));
}

static void write_legacy(const char *path,unsigned generation,time_t modified) {
    const char payload[]="legacy data";
    SaveHeader header={.tag={'M','P','S','1'},.stage="p1_l1-1",.room="p5",.generation=generation,
                       .signature=42,.bytes=sizeof payload,.crc=save_hash(payload,sizeof payload)};
    FILE *f=fopen(path,"wb");
    assert(f && fwrite(&header,sizeof header,1,f)==1 && fwrite(payload,sizeof payload,1,f)==1 && fclose(f)==0);
    struct utimbuf stamp={.actime=modified,.modtime=modified};
    assert(utime(path,&stamp)==0);
}

static void verify_latest_checkpoint(void) {
    const char data[]="state";
    assert(save_latest(0)==-1 && save_latest(SAVE_TRAINING)==-1);
    write_legacy("sdmc:/3ds/maxpayne/save00a.dat",19,1000000000);
    write_legacy("sdmc:/3ds/maxpayne/save03a.dat",1,1000000010);
    assert(save_latest(0)==3);

    assert(save_write(0,"p1_l1-2","startroom",42,data,sizeof data));
    assert(save_latest(0)==0);
    assert(save_write(3,"p1_l1-1","p5",42,data,sizeof data));
    assert(save_latest(0)==3);
    // Metadata resolves the order even when files share a timestamp.
    struct utimbuf same_time={.actime=1000000020,.modtime=1000000020};
    assert(utime("sdmc:/3ds/maxpayne/save00b.dat",&same_time)==0);
    assert(utime("sdmc:/3ds/maxpayne/save03b.dat",&same_time)==0);
    assert(save_latest(0)==3);

    assert(save_write(SAVE_TRAINING,"tutorial","startroom",42,data,sizeof data));
    assert(save_write(SAVE_BONUS,"end_combat","startroom",42,data,sizeof data));
    assert(save_loadable(SAVE_TRAINING) && save_loadable(SAVE_BONUS));
    assert(save_latest(0)==3 && save_latest(SAVE_TRAINING)==SAVE_TRAINING && save_latest(SAVE_BONUS)==SAVE_BONUS);
    assert(save_write(2,"tutorial","startroom",42,data,sizeof data));
    assert(save_latest(0)==3 && save_latest(SAVE_TRAINING)==2);
    SaveHeader latest;
    assert(save_info(3,&latest));
    damage_checkpoint(3,latest.generation);
    assert(save_latest(0)==0);
    write_legacy("sdmc:/3ds/maxpayne/save01a.dat",1,1000000030);
    assert(save_latest(0)==1);
    assert(save_write(1,"tutorial","startroom",42,data,sizeof data));
    assert(save_latest(0)==0 && save_latest(SAVE_TRAINING)==1);
    assert(save_info(1,&latest));
    damage_checkpoint(1,latest.generation);
    assert(save_latest(0)==1 && save_latest(SAVE_TRAINING)==2);
}

int main(void) {
    game_run.mode = DEAD_ON_ARRIVAL;
    assert(game_rules_enter("p1_l1-1"));
    assert(game_save_limit() == 5);
    game_run.saves = 5;
    game_run.remaining = 14;
    game_run.elapsed = 89;
    GameRun checkpoint = game_run;
    assert(game_rules_enter("p1_l1-2"));
    assert(game_run_valid(&checkpoint));
    assert(game_run.saves == 5 && game_run.remaining == 14);
    assert(game_rules_enter("p1_l1b-1"));
    assert(!game_run_valid(&checkpoint));
    assert(game_run.saves == 0 && game_run.remaining == 60 && game_run.elapsed == 89);
    game_run.mode = NEW_YORK_MINUTE;
    assert(game_kill_bonus() == 8);

    assert(save_unlocked_modes() == 1);
    assert(save_unlock_mode(HARD_BOILED));
    assert(save_unlock_mode(NEW_YORK_MINUTE));
    assert(save_unlocked_modes() == 11);
    assert(save_unlock_mode(DEAD_ON_ARRIVAL));
    assert(save_unlocked_modes() == 15);

    unsigned best;
    assert(save_race_time(0, &best) && best == 0);
    assert(save_race_time(300, &best) && best == 300);
    assert(save_race_time(240, &best) && best == 240);
    assert(save_race_time(310, &best) && best == 240);
    FILE *damaged = fopen("sdmc:/3ds/maxpayne/raceb.dat", "wb");
    assert(damaged && fputs("NYM", damaged) >= 0 && fclose(damaged) == 0);
    assert(save_race_time(0, &best) && best == 300);
    assert(save_race_time(200, &best) && best == 200);
    verify_latest_checkpoint();
    verify_checkpoint_recovery();
    return 0;
}
