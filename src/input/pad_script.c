/**
 * Scripted button presses for a virtual controller. See pad_script.h.
 */

#include "pad_script.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Digital bits, as XBOX_GAMEPAD_* in xinput_xbox.h. */
#define PAD_DPAD_UP     0x0001
#define PAD_DPAD_DOWN   0x0002
#define PAD_DPAD_LEFT   0x0004
#define PAD_DPAD_RIGHT  0x0008
#define PAD_START       0x0010
#define PAD_BACK        0x0020
#define PAD_LEFT_THUMB  0x0040
#define PAD_RIGHT_THUMB 0x0080

static const struct {
    const char *name;
    uint16_t digital;   /* bit in XBOX_GAMEPAD.wButtons, or 0 */
    int analog;         /* index into bAnalogButtons, or -1 */
} k_buttons[] = {
    { "START",  PAD_START,       -1 },
    { "BACK",   PAD_BACK,        -1 },
    { "UP",     PAD_DPAD_UP,     -1 },
    { "DOWN",   PAD_DPAD_DOWN,   -1 },
    { "LEFT",   PAD_DPAD_LEFT,   -1 },
    { "RIGHT",  PAD_DPAD_RIGHT,  -1 },
    { "LTHUMB", PAD_LEFT_THUMB,  -1 },
    { "RTHUMB", PAD_RIGHT_THUMB, -1 },
    { "A",      0, 0 },
    { "B",      0, 1 },
    { "X",      0, 2 },
    { "Y",      0, 3 },
    { "BLACK",  0, 4 },
    { "WHITE",  0, 5 },
    { "LT",     0, 6 },
    { "RT",     0, 7 },
};

/* Apply one button name, [name, name + len), to `held`. 0 if unknown. */
static int press(const char *name, size_t len, XboxPadHeld *held)
{
    size_t i, k;

    for (i = 0; i < sizeof(k_buttons) / sizeof(k_buttons[0]); i++) {
        const char *want = k_buttons[i].name;
        if (strlen(want) != len)
            continue;
        for (k = 0; k < len; k++)
            if (toupper((unsigned char)name[k]) != want[k])
                break;
        if (k != len)
            continue;
        held->buttons |= k_buttons[i].digital;
        if (k_buttons[i].analog >= 0)
            held->analog[k_buttons[i].analog] = 255;
        return 1;
    }
    return 0;
}

/* Parse one "SECONDS=NAMES" event from [p, end). */
static int parse_event(const char *p, const char *end, double *t, XboxPadHeld *held)
{
    const char *eq = memchr(p, '=', (size_t)(end - p));
    char number[32];
    char *stop;
    size_t n;

    if (!eq)
        return 0;
    while (p < eq && isspace((unsigned char)*p))
        p++;
    n = (size_t)(eq - p);
    while (n && isspace((unsigned char)p[n - 1]))
        n--;
    if (n == 0 || n >= sizeof(number))
        return 0;
    memcpy(number, p, n);
    number[n] = '\0';
    *t = strtod(number, &stop);
    if (*stop != '\0' || *t < 0)
        return 0;

    memset(held, 0, sizeof(*held));
    p = eq + 1;
    while (p < end && isspace((unsigned char)*p))
        p++;
    if (p == end)
        return 1;                   /* "SECONDS=" releases everything */

    for (;;) {
        const char *plus = memchr(p, '+', (size_t)(end - p));
        const char *stop_at = plus ? plus : end;
        while (p < stop_at && isspace((unsigned char)*p))
            p++;
        n = (size_t)(stop_at - p);
        while (n && isspace((unsigned char)p[n - 1]))
            n--;
        /* An empty name is "A++B", "+A" or "A+". */
        if (n == 0 || !press(p, n, held))
            return 0;
        if (!plus)
            return 1;
        p = plus + 1;
    }
}

int xbox_pad_script_parse(const char *text, XboxPadScript *out)
{
    const char *p = text;
    int i, j;

    memset(out, 0, sizeof(*out));
    if (!text)
        return 0;

    while (*p) {
        const char *end = p;
        while (*end && *end != ';' && *end != ',')
            end++;
        {
            const char *q = p;
            while (q < end && isspace((unsigned char)*q))
                q++;
            if (q < end) {
                if (out->count == XBOX_PAD_SCRIPT_MAX_EVENTS
                        || !parse_event(p, end, &out->time[out->count],
                                        &out->held[out->count])) {
                    memset(out, 0, sizeof(*out));
                    return -1;
                }
                out->count++;
            }
        }
        p = *end ? end + 1 : end;
    }

    /* Insertion sort, stable, so events written out of order still apply in
     * time order and equal times keep the order they were written in. */
    for (i = 1; i < out->count; i++) {
        double t = out->time[i];
        XboxPadHeld h = out->held[i];
        for (j = i; j > 0 && out->time[j - 1] > t; j--) {
            out->time[j] = out->time[j - 1];
            out->held[j] = out->held[j - 1];
        }
        out->time[j] = t;
        out->held[j] = h;
    }
    return out->count;
}

XboxPadHeld xbox_pad_script_at(const XboxPadScript *script, double seconds)
{
    XboxPadHeld none;
    int i;

    memset(&none, 0, sizeof(none));
    for (i = script->count - 1; i >= 0; i--)
        if (script->time[i] <= seconds)
            return script->held[i];
    return none;
}
