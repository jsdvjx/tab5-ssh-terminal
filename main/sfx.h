// Procedural UI sound effects (speaker via BSP codec). Fire-and-forget;
// playback happens on a dedicated low-prio task.
#pragma once

enum {
    SFX_CLICK,   // typewriter key
    SFX_TICK,    // shuffle beat
    SFX_DING,    // logo settle
    SFX_COUNT,
};

void sfx_init(void);       // safe to call even if the speaker is absent
void sfx_play(int id);
