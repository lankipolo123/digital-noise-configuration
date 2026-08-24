# TX Lite

Single-page Windows GUI for the 16-channel SDR/RF module, controlled over
RS422. A from-scratch C/Win32 rewrite of [sdr_controller](https://github.com/lankipolo123/sdr_controller),
consolidating its Dashboard + Device Control pages (minus the Communication
page's terminal log and activity chart) into one window.

No Qt, no Python runtime, no vendor DLL, no pyserial - just `user32.dll`,
`gdi32.dll`, `kernel32.dll`, `advapi32.dll` (all part of Windows itself).
Compiled size is currently **~56 KB**.

## Building

Requires mingw-w64 (get it via [MSYS2](https://www.msys2.org/), then run
this from the "MSYS2 MinGW x64" shell, or any shell with mingw-w64's
`bin` on `PATH`):

```
build.bat
```

produces `tx_lite.exe`.

## Layout

- `src/protocol.h` / `.c` - RS422 frame format (build/parse), ported from
  `sdr_controller/protocol/`. No malloc, no Windows dependency.
- `src/serial_port.h` / `.c` - raw `CreateFile`/`ReadFile`/`WriteFile`
  COM port I/O + registry port enumeration. No pyserial.
- `src/connection.h` / `.c` - connection lifecycle + framing on top of
  `serial_port`, ported from `ConnectionController`. Polled from a
  `WM_TIMER` tick instead of a reader thread.
- `src/device.h` / `.c` - device state + command orchestration (2s response
  timeout, optimistic output toggle, ACK-gated signal/address changes),
  ported from `DeviceController` + `DeviceState`.
- `src/main.c` / `resource.h` - the Win32 window itself.

## Known simplifications vs. the Python reference

- No config persistence (COM port/baud/parity/module address/auto-connect
  aren't saved between runs yet - every launch starts from the same
  defaults the reference app ships with).
- No logging (the excluded Communication page owned that; nothing here
  writes a log file).
- Plain `-` instead of the reference's em dash for "no value yet", to
  avoid pulling in wide-character/codepage handling for one character.

## Verification

`src/test_protocol.c` mirrors `sdr_controller/protocol/test_protocol.py`'s
assertions and confirms byte-identical frame output. The whole app has
also been built and exercised under Wine (window creation, control
interaction, the optimistic-apply/revert path, the unconfirmed-value
confirmation dialog) - see commit history for details. It has not yet
been tested against real hardware.
