/*
 * Per-title switch for real guest-KEVENT waits and the NO_OPERATION notify.
 *
 * With it on, a KEVENT the title built in guest memory gets real set/wait
 * semantics (kernel_sync.c) and NO_OPERATION with a non-zero parameter raises
 * the PGRAPH notify interrupt whose handler sets that event (nv2a_core.c).
 * Burnout's D3D needs both under async PFIFO.
 *
 * It is off by default because a title may carry fixes written for the old
 * behaviour, where those waits failed at once. Breakdown's spin fixes at
 * 0x001C0C47 / 0x001C0C6F do, and with real waits its push-buffer space wait
 * hangs (HeroLab Burnout task 7b9e050b).
 *
 * A title turns it on with the compile definition XBOXRECOMP_GUEST_KEVENTS=1,
 * set before add_subdirectory(xboxrecomp) so the runtime libraries see it.
 * The environment variable of the same name (0 or 1) overrides it at run time.
 */
#ifndef XBOX_GUEST_SYNC_H
#define XBOX_GUEST_SYNC_H

#include <stdbool.h>
#include <stdlib.h>

#ifndef XBOXRECOMP_GUEST_KEVENTS
#define XBOXRECOMP_GUEST_KEVENTS 0
#endif

static inline bool xbox_guest_kevents_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("XBOXRECOMP_GUEST_KEVENTS");
        on = (e && *e) ? e[0] != '0' : XBOXRECOMP_GUEST_KEVENTS != 0;
    }
    return on != 0;
}

#endif /* XBOX_GUEST_SYNC_H */
