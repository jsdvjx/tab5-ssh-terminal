// Terminal-style boot animation (~3.3s, self-deleting). Design source:
// tools/bootanim_preview.py. Call right after the display is up; boot
// continues underneath on the LVGL top layer.
#pragma once

void boot_anim_start(void);

// Release the splash once boot is complete and the UI is up; it fades out.
void boot_anim_finish(void);
