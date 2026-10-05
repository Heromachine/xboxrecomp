/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include "pad_script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };

/* A disconnected port is probed again no sooner than this. XInputGetState on
 * an empty port is slow on real Windows, and titles ask every frame. */
#define PROBE_INTERVAL_MS 1000
static DWORD g_next_probe_ms[XBOX_MAX_CONTROLLERS];

/* The packet number a title sees changes exactly when the state does, whatever
 * the source -- host pad, keyboard stand-in or script. */
static XBOX_GAMEPAD g_last_pad[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

static int g_setup_done;
static int g_keyboard_pad = 1;          /* XBOXRECOMP_KEYBOARD_PAD=0 turns it off */
static int g_input_trace;
static XboxPadScript g_script;          /* XBOXRECOMP_PAD_SCRIPT */
static LARGE_INTEGER g_t0, g_freq;

static void input_setup(void)
{
    const char *e;

    if (g_setup_done)
        return;
    g_setup_done = 1;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    e = getenv("XBOXRECOMP_KEYBOARD_PAD");
    if (e && e[0] == '0')
        g_keyboard_pad = 0;

    e = getenv("XBOXRECOMP_INPUT_TRACE");
    g_input_trace = e && e[0] == '1';

    e = getenv("XBOXRECOMP_PAD_SCRIPT");
    if (e && *e) {
        int n = xbox_pad_script_parse(e, &g_script);
        if (n < 0)
            fprintf(stderr, "[INPUT] XBOXRECOMP_PAD_SCRIPT does not parse; ignoring it\n");
        else
            fprintf(stderr, "[INPUT] pad script: %d event(s) on port 0\n", n);
    }
    fprintf(stderr, "[INPUT] keyboard stand-in pad on port 0 when no controller is: %s\n",
            g_keyboard_pad ? "on" : "off");
}

/* Seconds since the input layer was first used -- early in boot, when the
 * title initialises its devices. XBOXRECOMP_PAD_SCRIPT times count from here. */
static double input_seconds(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart;
}

/* Port 0 has a controller even with no host pad when either stand-in is on. */
static int port0_virtual(void)
{
    return g_keyboard_pad || g_script.count > 0;
}

/* A host XInput pad in Xbox layout. */
static DWORD read_xinput(DWORD port, XBOX_GAMEPAD *pad)
{
    XINPUT_STATE xi_state;
    DWORD result = XInputGetState(port, &xi_state);

    if (result != ERROR_SUCCESS)
        return result;

    pad->wButtons = xi_state.Gamepad.wButtons & 0x00FF;
    pad->bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pad->bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pad->bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pad->bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pad->bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pad->bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pad->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pad->bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;
    pad->sThumbLX = xi_state.Gamepad.sThumbLX;
    pad->sThumbLY = xi_state.Gamepad.sThumbLY;
    pad->sThumbRX = xi_state.Gamepad.sThumbRX;
    pad->sThumbRY = xi_state.Gamepad.sThumbRY;
    return ERROR_SUCCESS;
}

/* The keyboard stand-in, read only while one of this process's windows has
 * focus so typing in another window doesn't press buttons. The layout is in
 * README.md. */
static void read_keyboard(XBOX_GAMEPAD *pad)
{
    static const struct { int vk; WORD digital; int analog; } keys[] = {
        { VK_RETURN, XBOX_GAMEPAD_START,       -1 },
        { VK_ESCAPE, XBOX_GAMEPAD_BACK,        -1 },
        { VK_UP,     XBOX_GAMEPAD_DPAD_UP,     -1 },
        { VK_DOWN,   XBOX_GAMEPAD_DPAD_DOWN,   -1 },
        { VK_LEFT,   XBOX_GAMEPAD_DPAD_LEFT,   -1 },
        { VK_RIGHT,  XBOX_GAMEPAD_DPAD_RIGHT,  -1 },
        { 'Z',       XBOX_GAMEPAD_LEFT_THUMB,  -1 },
        { 'C',       XBOX_GAMEPAD_RIGHT_THUMB, -1 },
        { VK_SPACE,  0, XBOX_BUTTON_A },
        { VK_BACK,   0, XBOX_BUTTON_B },
        { 'F',       0, XBOX_BUTTON_X },
        { 'R',       0, XBOX_BUTTON_Y },
        { 'G',       0, XBOX_BUTTON_BLACK },
        { 'T',       0, XBOX_BUTTON_WHITE },
        { 'Q',       0, XBOX_BUTTON_LTRIGGER },
        { 'E',       0, XBOX_BUTTON_RTRIGGER },
    };
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    size_t i;

    if (!fg)
        return;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId())
        return;

#define KEY_DOWN(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!KEY_DOWN(keys[i].vk))
            continue;
        pad->wButtons |= keys[i].digital;
        if (keys[i].analog >= 0)
            pad->bAnalogButtons[keys[i].analog] = 255;
    }
    /* Sticks at full deflection; the Xbox Y axis points up. */
    pad->sThumbLX = (SHORT)((KEY_DOWN('D') ? 32767 : 0) - (KEY_DOWN('A') ? 32767 : 0));
    pad->sThumbLY = (SHORT)((KEY_DOWN('W') ? 32767 : 0) - (KEY_DOWN('S') ? 32767 : 0));
    pad->sThumbRX = (SHORT)((KEY_DOWN('L') ? 32767 : 0) - (KEY_DOWN('J') ? 32767 : 0));
    pad->sThumbRY = (SHORT)((KEY_DOWN('I') ? 32767 : 0) - (KEY_DOWN('K') ? 32767 : 0));
#undef KEY_DOWN
}

void xbox_InputInit(void)
{
    input_setup();
    xbox_InputConnectedMask();
}

DWORD xbox_InputConnectedMask(void)
{
    DWORD mask = 0, now = GetTickCount();

    input_setup();
    for (DWORD port = 0; port < XBOX_MAX_CONTROLLERS; port++) {
        if (g_controller_connected[port] || now >= g_next_probe_ms[port]) {
            XINPUT_STATE state;
            g_controller_connected[port] =
                (XInputGetState(port, &state) == ERROR_SUCCESS);
            if (!g_controller_connected[port])
                g_next_probe_ms[port] = now + PROBE_INTERVAL_MS;
        }
        if (g_controller_connected[port])
            mask |= 1u << port;
    }
    if (port0_virtual())
        mask |= 1u;
    return mask;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XBOX_GAMEPAD pad;
    DWORD result;

    input_setup();
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(&pad, 0, sizeof(pad));
    result = read_xinput(dwPort, &pad);
    g_controller_connected[dwPort] = (result == ERROR_SUCCESS);
    if (result != ERROR_SUCCESS) {
        if (dwPort != 0 || !port0_virtual())
            return result;
        if (g_keyboard_pad)
            read_keyboard(&pad);
    }

    if (dwPort == 0 && g_script.count > 0) {
        XboxPadHeld held = xbox_pad_script_at(&g_script, input_seconds());
        pad.wButtons |= held.buttons;
        for (int i = 0; i < 8; i++)
            if (held.analog[i] > pad.bAnalogButtons[i])
                pad.bAnalogButtons[i] = held.analog[i];
    }

    if (dwPort == 0 && g_input_trace) {
        static int last_a = -1, last_lx = 2, last_ly = 2;
        static WORD last_dpad = 0xFFFF;
        static int last_source = -1;
        int a = pad.bAnalogButtons[XBOX_BUTTON_A] > 0;
        int lx = pad.sThumbLX < -12000 ? -1 : pad.sThumbLX > 12000 ? 1 : 0;
        int ly = pad.sThumbLY < -12000 ? -1 : pad.sThumbLY > 12000 ? 1 : 0;
        WORD dpad = pad.wButtons & 0x000F;
        int source = result == ERROR_SUCCESS ? 1 : g_keyboard_pad ? 0 : 2;
        if (a != last_a || lx != last_lx || ly != last_ly ||
            dpad != last_dpad || source != last_source) {
            fprintf(stderr, "[INPUT TRACE] ms=%llu source=%s dpad=%04X "
                    "A=%u LX=%d LY=%d packet=%lu\n",
                    (unsigned long long)GetTickCount64(),
                    source == 1 ? "controller" : source == 0 ? "keyboard" : "script",
                    dpad, pad.bAnalogButtons[XBOX_BUTTON_A],
                    pad.sThumbLX, pad.sThumbLY,
                    (unsigned long)g_packet[dwPort]);
            last_a = a; last_lx = lx; last_ly = ly;
            last_dpad = dpad; last_source = source;
        }
    }

    if (g_packet[dwPort] == 0 || memcmp(&pad, &g_last_pad[dwPort], sizeof(pad)) != 0) {
        g_last_pad[dwPort] = pad;
        g_packet[dwPort]++;
    }

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = g_packet[dwPort];
    pState->Gamepad = pad;
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return (xbox_InputConnectedMask() >> dwPort) & 1;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    for (int i = 0; i < SDL_NumJoysticks() && slot < XBOX_MAX_CONTROLLERS; i++) {
        if (!SDL_IsGameController(i))
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
}

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    open_controllers();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

/* No keyboard stand-in or XBOXRECOMP_PAD_SCRIPT on this backend yet. */
DWORD xbox_InputConnectedMask(void)
{
    DWORD mask = 0;
    open_controllers();
    for (DWORD port = 0; port < XBOX_MAX_CONTROLLERS; port++) {
        SDL_GameController *c = g_pads[port];
        g_controller_connected[port] = c && SDL_GameControllerGetAttached(c);
        if (g_controller_connected[port])
            mask |= 1u << port;
    }
    return mask;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
