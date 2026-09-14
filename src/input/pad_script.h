/**
 * Scripted button presses for a virtual controller.
 *
 * XBOXRECOMP_PAD_SCRIPT lets a run press buttons without anyone at the
 * keyboard, so a test can drive a title past "PRESS START" and check where it
 * lands. The text is a list of events, each "SECONDS=BUTTONS", separated by
 * ';' or ','. BUTTONS is '+'-separated names, held from that time until the
 * next event; an empty list releases everything:
 *
 *     XBOXRECOMP_PAD_SCRIPT="135=START;135.2=;140=A;140.2="
 *
 * Names (any case): START BACK A B X Y BLACK WHITE LT RT UP DOWN LEFT RIGHT
 * LTHUMB RTHUMB.
 *
 * Portable C with no platform headers, so the host test suite compiles it
 * directly (tools/recomp/test_pad_script.py).
 */

#ifndef XBOX_PAD_SCRIPT_H
#define XBOX_PAD_SCRIPT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_PAD_SCRIPT_MAX_EVENTS 64

/* What a pad holds: XBOX_GAMEPAD's digital bits and its eight analog buttons
 * (A, B, X, Y, Black, White, left trigger, right trigger), 0 or 255. */
typedef struct XboxPadHeld {
    uint16_t buttons;
    uint8_t  analog[8];
} XboxPadHeld;

typedef struct XboxPadScript {
    int count;
    double      time[XBOX_PAD_SCRIPT_MAX_EVENTS];   /* seconds, ascending */
    XboxPadHeld held[XBOX_PAD_SCRIPT_MAX_EVENTS];
} XboxPadScript;

/* Parse `text` into `out`, sorted by time. Returns the number of events, or
 * -1 with `out->count` 0 if any part of it doesn't parse -- a script that is
 * half-applied would press the wrong buttons silently. */
int xbox_pad_script_parse(const char *text, XboxPadScript *out);

/* What the script holds at `seconds`: the last event at or before it, or
 * nothing before the first. */
XboxPadHeld xbox_pad_script_at(const XboxPadScript *script, double seconds);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_PAD_SCRIPT_H */
