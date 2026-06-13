// Voice input: Ctrl+Alt+V records from the ES7210 mics, ships a WAV to a
// whisper server (settings_t.voice_url) and types the transcription into the
// SSH session via ime_filter_commit_text(). Esc while recording cancels.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create the worker task. Mic codec is opened lazily on first use.
void voice_input_init(settings_t *s);

// Ctrl+Alt+V: start recording / stop+transcribe. Safe from the HID task —
// just posts to the worker's queue, never blocks.
void voice_input_toggle(void);

// Esc while recording: discard the take.
void voice_input_cancel(void);
bool voice_input_is_recording(void);

// Synchronous: record `ms` of audio and return a malloc'd WAV (44-byte
// header + 16k/16-bit/mono PCM). Caller frees. Used by the /api/rec debug
// endpoint and `voice test`. Returns false if the mic is unavailable/busy.
bool voice_input_record_wav(int ms, uint8_t **out, size_t *out_len);

// Synchronous full-pipeline check for the `voice test` shell command:
// records `ms`, uploads, prints the result with printf(). Returns 0 on
// success (shell exit code).
int voice_input_test(int ms);

#ifdef __cplusplus
}
#endif
