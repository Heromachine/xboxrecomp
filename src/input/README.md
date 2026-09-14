# xbox_input — Xbox Gamepad to XInput

Maps the Xbox controller API to Windows XInput. The original Xbox used `XInputGetState` with a slightly different structure layout than the XInput API on Windows. This layer translates between them.

## Files

| File | LOC | Purpose |
|------|-----|---------|
| `xinput_xbox.h` | 123 | Public header — types, button constants, function prototypes |
| `xinput_device.c` | 408 | Implementation: Windows XInput backend (with the keyboard stand-in and scripted presses), SDL2 backend on POSIX |
| `pad_script.c/.h` | 224 | `XBOXRECOMP_PAD_SCRIPT` parser, portable C (tested by `tools/recomp/test_pad_script.py`) |

## Wiring a title

On the Xbox, `XInputGetState` and its siblings are not kernel exports. They are
XDK library code linked into the title's XPP section, and they drive the USB
host controller, which the runtime does not emulate. A recompiled title never
sees a controller until those entry points are replaced with this layer.

Find them in the title, then define them in its `recomp_manual.c` so
`--exclude-manual` drops the recompiled bodies, and register each in
`recomp_lookup_manual()` too. Callers of a manually defined function reach it
through the dispatch lookup. Breakdown's overrides are the worked example:
`XGetDeviceChanges` reports ports from `xbox_InputConnectedMask()`, `XInputOpen`
hands out a handle per port, and `XInputGetState`, `XInputGetCapabilities` and
`XInputSetState` read and write guest memory in the XDK layouts through this
layer.

## Quick Start

```c
#include "xinput_xbox.h"

// Initialize input system
xbox_InputInit();

// Poll controller state (port 0-3)
XBOX_INPUT_STATE state;
if (xbox_InputGetState(0, &state) == 0) {
    // Digital buttons
    if (state.Gamepad.wButtons & XBOX_GAMEPAD_START)
        pause_game();

    // Analog buttons (0-255 pressure)
    uint8_t trigger_r = state.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER];
    if (trigger_r > XBOX_ANALOG_BUTTON_THRESHOLD)
        accelerate(trigger_r / 255.0f);

    // Stick axes (-32768 to +32767)
    float steer = state.Gamepad.sThumbLX / 32768.0f;
}

// Vibration feedback
XBOX_VIBRATION vib = { .wLeftMotorSpeed = 32000, .wRightMotorSpeed = 16000 };
xbox_InputSetState(0, &vib);
```

## API

```c
// Initialize (call once at startup)
void xbox_InputInit(void);

// Poll controller state (returns 0 on success, non-zero if disconnected)
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

// Set vibration motors
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

// Check if controller is connected
BOOL xbox_InputIsConnected(DWORD dwPort);

// Bit n set for each port with a controller (host pad, or on port 0 the
// keyboard stand-in or a pad script) -- what XGetDeviceChanges should report
DWORD xbox_InputConnectedMask(void);

// Query controller capabilities
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);
```

## Types

```c
typedef struct {
    WORD  wButtons;                     // Digital button bitmask
    BYTE  bAnalogButtons[8];            // Analog button pressure (0-255)
    SHORT sThumbLX, sThumbLY;           // Left stick (-32768 to +32767)
    SHORT sThumbRX, sThumbRY;           // Right stick
} XBOX_GAMEPAD;

typedef struct {
    DWORD dwPacketNumber;               // Increments on state change
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct {
    WORD wLeftMotorSpeed;               // 0-65535
    WORD wRightMotorSpeed;              // 0-65535
} XBOX_VIBRATION;
```

## Button Constants

### Digital Buttons (wButtons bitmask)

```c
XBOX_GAMEPAD_DPAD_UP         0x0001
XBOX_GAMEPAD_DPAD_DOWN       0x0002
XBOX_GAMEPAD_DPAD_LEFT       0x0004
XBOX_GAMEPAD_DPAD_RIGHT      0x0008
XBOX_GAMEPAD_START            0x0010
XBOX_GAMEPAD_BACK             0x0020
XBOX_GAMEPAD_LEFT_THUMB       0x0040    // Left stick click
XBOX_GAMEPAD_RIGHT_THUMB      0x0080    // Right stick click
```

### Analog Buttons (bAnalogButtons[] indices)

The original Xbox had pressure-sensitive face buttons (0-255):

```c
XBOX_BUTTON_A          0    // Also used for "boost" in racing games
XBOX_BUTTON_B          1
XBOX_BUTTON_X          2
XBOX_BUTTON_Y          3
XBOX_BUTTON_BLACK      4    // No equivalent on modern controllers
XBOX_BUTTON_WHITE      5    // No equivalent on modern controllers
XBOX_BUTTON_LTRIGGER   6    // Left trigger
XBOX_BUTTON_RTRIGGER   7    // Right trigger

XBOX_ANALOG_BUTTON_THRESHOLD  30   // Recommended press threshold
```

### Xbox → Modern Controller Mapping

| Xbox Button | XInput Equivalent | Notes |
|-------------|------------------|-------|
| A | A | Green button |
| B | B | Red button |
| X | X | Blue button |
| Y | Y | Yellow button |
| Black | Right Bumper | Mapped to RB |
| White | Left Bumper | Mapped to LB |
| L Trigger | Left Trigger | Analog 0-255 |
| R Trigger | Right Trigger | Analog 0-255 |
| Start | Start/Menu | |
| Back | Back/View | |
| D-pad | D-pad | Digital only |
| L Stick | L Stick | Click = L3 |
| R Stick | R Stick | Click = R3 |

## Keyboard stand-in (Windows backend)

With no controller on port 0, the keyboard acts as one while one of the
game's windows has focus. `XBOXRECOMP_KEYBOARD_PAD=0` turns it off. A real
controller on port 0 always takes over.

| Pad | Key |
|-----|-----|
| Start | Enter |
| Back | Esc |
| D-pad | Arrow keys |
| Left stick | W A S D |
| Right stick | I J K L |
| A / B / X / Y | Space / Backspace / F / R |
| Black / White | G / T |
| Left / right trigger | Q / E |
| Left / right stick click | Z / C |

## Scripted presses (Windows backend)

`XBOXRECOMP_PAD_SCRIPT` presses buttons on port 0 at set times, so a run can
get past a title screen with nobody at the keyboard. Events are
`SECONDS=BUTTONS`, separated by `;` or `,`. The buttons are `+`-separated and
held until the next event, and an empty list releases them all. Times count
from the title's first input call, early in boot.

`XBOXRECOMP_PAD_SCRIPT="132=START;132.3=;140=DOWN;140.2=;143=A;143.2="`

Names: `START BACK A B X Y BLACK WHITE LT RT UP DOWN LEFT RIGHT LTHUMB RTHUMB`.
A script that doesn't parse is ignored as a whole, with a message on stderr.
It is ORed onto whatever port 0 reports, so it also works alongside a
connected controller.

The packet number a title reads changes exactly when the state does, whichever
source the state came from.

## Ports

```c
#define XBOX_MAX_CONTROLLERS  4   // Ports 0-3
```

The Xbox supports 4 controllers. Each port can have a controller with optional memory units and other accessories. This layer only handles the gamepad; memory unit emulation is not needed for recompiled games.
