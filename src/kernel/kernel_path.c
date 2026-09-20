/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 1 = under save_dir, 0 = under game_dir    */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition1\\",    0, NULL,         NULL          },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 1, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/* ======================================================================== */
#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static BOOL  s_initialized = FALSE;

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\xboxrecomp", save_base);
        } else {
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    /* Create the save-side directories. T:/U:/Z: map into subdirectories of
     * save_dir, and a title that opens a file there with a create disposition
     * fails if the parent does not exist -- which reads as "cannot create save
     * file" and sends the title down its init-failure path. Halo asserts
     * exactly that at saved games/game_state_xbox.c:97 and then unwinds,
     * clearing global_d3d_device on the way out, so a missing directory
     * surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        static const WCHAR *subs[] = { L"TitleData", L"UserData", L"Cache" };
        WCHAR dir[MAX_PATH];
        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        for (int i = 0; i < 3; i++) {
            swprintf_s(dir, MAX_PATH, L"%s\\%s", s_save_dir, subs[i]);
            SHCreateDirectoryExW(NULL, dir, NULL);
        }
    }

    /* Back \Device\Harddisk0\partition0 with a real 512KB file.
     *
     * Partition0 is not a filesystem -- it is a raw kernel-reserved region at
     * the start of the Xbox HDD holding disk configuration (boot counters,
     * manufacturing flags, and at offset 0x800 the cache-partition allocation
     * table). Titles open it directly and read/write that table to claim a
     * cache partition for themselves; Breakdown does exactly this in
     * sub_001ABB8E, and without a backing device the open fails, the title
     * raises a fatal error and reboots to the dashboard.
     *
     * Zero-filled is correct and sufficient: the title validates the table's
     * magic/version/signature and rebuilds it from scratch when they don't
     * match, so an empty region exercises the same path a freshly formatted
     * disk would. Created once, then persisted, so an allocation survives
     * across runs. */
    {
        WCHAR p0[MAX_PATH];
        swprintf_s(p0, MAX_PATH, L"%s\\partition0.bin", s_save_dir);
        if (GetFileAttributesW(p0) == INVALID_FILE_ATTRIBUTES) {
            HANDLE h = CreateFileW(p0, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz; sz.QuadPart = 0x80000;   /* 512 KB */
                SetFilePointerEx(h, sz, NULL, FILE_BEGIN);
                SetEndOfFile(h);
                CloseHandle(h);
            }
        }
    }

    /* Back the raw \Device\CdRom0 device with a small file.
     *
     * Titles open the DVD drive as a device -- not as a filesystem -- to
     * interrogate the media: Burnout does so in sub_00018C9F and immediately
     * issues a SCSI MODE SENSE(10) through IOCTL_SCSI_PASS_THROUGH_DIRECT,
     * which kernel_file.c answers. Without a backing object the open fails
     * with STATUS_OBJECT_PATH_NOT_FOUND, the title treats that as "no disc"
     * and returns to the dashboard.
     *
     * This is a handle placeholder, NOT a disc image: it exists so the device
     * can be opened and IOCTLs issued against it. Raw sector reads of the disc
     * are not supported, and a title attempting one will read zeroes -- which
     * is why it is one sector long rather than pretending to be a disc. */
    {
        WCHAR cd[MAX_PATH];
        swprintf_s(cd, MAX_PATH, L"%s\\cdrom0.dev", s_save_dir);
        if (GetFileAttributesW(cd) == INVALID_FILE_ATTRIBUTES) {
            HANDLE h = CreateFileW(cd, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz; sz.QuadPart = 2048;   /* one DVD sector */
                SetFilePointerEx(h, sz, NULL, FILE_BEGIN);
                SetEndOfFile(h);
                CloseHandle(h);
            }
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S", s_game_dir, s_save_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    const char*  sub_dir   = NULL;
    int          skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    /* Raw partition0 is a single binary region, not a directory tree, so it
     * maps to one backing file rather than through the prefix rules below
     * (which all append a remainder path). See xbox_path_init. */
    if (match_prefix(xbox_path, "\\Device\\Harddisk0\\partition0")) {
        fprintf(stderr, "  [PATH] %s -> partition0.bin (raw disk config region)\n",
                xbox_path);
        fflush(stderr);
        swprintf_s(host_path_buf, buf_size, L"%s\\partition0.bin", s_save_dir);
        return TRUE;
    }

    /* Raw CD-ROM device. Same bare-vs-separator convention as the partitions
     * above: "\Device\CdRom0" with nothing after it addresses the DEVICE, so
     * it maps to the backing object and is where SCSI pass-through IOCTLs are
     * aimed. "\Device\CdRom0\..." is the FILESYSTEM on the disc and is left
     * to the prefix rules below, which map it to the game directory.
     *
     * The explicit end-of-string test is what keeps those two apart; matching
     * the prefix alone would swallow every file path on the disc. */
    if ((skip = match_prefix(xbox_path, "\\Device\\CdRom0")) != 0 &&
        xbox_path[skip] == '\0') {
        fprintf(stderr, "  [PATH] %s -> cdrom0.dev (raw CD-ROM device)\n", xbox_path);
        fflush(stderr);
        swprintf_s(host_path_buf, buf_size, L"%s\\cdrom0.dev", s_save_dir);
        return TRUE;
    }

    /* Cache partitions. Which partition a title gets is decided at runtime by
     * the allocation table it reads out of partition0 (Breakdown claims one in
     * sub_001ABB8E and lands on Partition5), so the number cannot be baked
     * into the rule table -- match any of them and give each its own
     * directory. Partition1 is excluded: it is the title's own game/user data
     * and keeps its existing rule below. */
    if ((skip = match_prefix(xbox_path, "\\Device\\Harddisk0\\Partition")) != 0) {
        const char* p = xbox_path + skip;
        int n = 0, ndigits = 0;
        while (p[ndigits] >= '0' && p[ndigits] <= '9') {
            n = n * 10 + (p[ndigits] - '0');
            ndigits++;
        }
        if (ndigits > 0 && n != 1) {
            const char* rem = p + ndigits;
            WCHAR rem_w[MAX_PATH], dir[MAX_PATH];

            if (*rem == '\0') {
                /* No trailing separator: this addresses the raw partition
                 * device, not the filesystem on it. A title formatting a
                 * freshly allocated cache partition writes a FATX superblock
                 * and FAT tables straight to this device (Breakdown does so in
                 * sub_001AE166), so it needs a real backing image rather than
                 * the directory the filesystem view maps to. Sparse, so the
                 * nominal partition size costs only what is actually written. */
                WCHAR img[MAX_PATH];
                swprintf_s(img, MAX_PATH, L"%s\\Partition%d.img", s_save_dir, n);
                if (GetFileAttributesW(img) == INVALID_FILE_ATTRIBUTES) {
                    HANDLE h = CreateFileW(img, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                           FILE_ATTRIBUTE_NORMAL, NULL);
                    if (h != INVALID_HANDLE_VALUE) {
                        LARGE_INTEGER sz;
                        sz.QuadPart = (LONGLONG)XBOX_CACHE_PARTITION_BYTES;
                        SetFilePointerEx(h, sz, NULL, FILE_BEGIN);
                        SetEndOfFile(h);
                        CloseHandle(h);
                    }
                }
                fprintf(stderr, "  [PATH] %s -> Partition%d.img (raw partition device)\n",
                        xbox_path, n);
                fflush(stderr);
                swprintf_s(host_path_buf, buf_size, L"%s", img);
                return TRUE;
            }

            if (*rem == '\\') rem++;
            swprintf_s(dir, MAX_PATH, L"%s\\Partition%d", s_save_dir, n);
            SHCreateDirectoryExW(NULL, dir, NULL);

            if (*rem) {
                MultiByteToWideChar(CP_ACP, 0, rem, -1, rem_w, MAX_PATH);
                for (WCHAR* q = rem_w; *q; q++)
                    if (*q == L'/') *q = L'\\';
                swprintf_s(host_path_buf, buf_size, L"%s\\%s", dir, rem_w);
            } else {
                /* The partition root itself -- no trailing separator, so it
                 * opens cleanly as a directory. */
                swprintf_s(host_path_buf, buf_size, L"%s", dir);
            }
            fprintf(stderr, "  [PATH] %s -> cache Partition%d\n", xbox_path, n);
            fflush(stderr);
            return TRUE;
        }
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_win;
            goto translate;
        }
    }

    /* TEMP TRACE (2026-09-03): xbox_log(WARN) is filtered by default, so this
     * silent passthrough was invisible -- the path is passed through
     * unchanged and CreateFileW fails on it for real, surfacing only as an
     * opaque STATUS_OBJECT_PATH_NOT_FOUND several layers up. See HeroLab
     * task 1b0f5bf7-5d54-4fb6-a89f-1ba04ad8969a. */
    fprintf(stderr, "  [PATH] UNRECOGNIZED (passthrough, will likely fail): %s\n", xbox_path);
    fflush(stderr);
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    MultiByteToWideChar(CP_ACP, 0, xbox_path, -1, host_path_buf, buf_size);
    return TRUE;

translate:
    fprintf(stderr, "  [PATH] %s\n", xbox_path);
    fflush(stderr);
    {
        WCHAR remainder_wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, remainder, -1, remainder_wide, MAX_PATH);

        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (sub_dir) {
            WCHAR sub_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, sub_dir, -1, sub_wide, MAX_PATH);
            swprintf_s(host_path_buf, buf_size, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);

            WCHAR dir_path[MAX_PATH];
            swprintf_s(dir_path, MAX_PATH, L"%s%s", base_dir, sub_wide);
            CreateDirectoryW(s_save_dir, NULL);
            CreateDirectoryW(dir_path, NULL);
        } else {
            swprintf_s(host_path_buf, buf_size, L"%s\\%s", base_dir, remainder_wide);
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static BOOL s_initialized = FALSE;

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/Burnout 3 Takedown", cwd);
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/burnout3", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/burnout3",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_posix;
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    return TRUE;

translate:
    {
        char remainder_posix[MAX_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}

#endif /* _WIN32 */
