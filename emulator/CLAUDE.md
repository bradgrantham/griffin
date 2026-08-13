# Debugging the emulator unattended (Claude Code)

The default pty console + SDL window can't be driven or read by an automated agent, and the emulator otherwise runs forever. Use these `emulator` flags (added for exactly this) to run headless, script I/O, capture output, and self-terminate — no `timeout`/perl wrapper needed:

* `--console-stdio` — DUART Channel A on stdin/stdout instead of a pty, so you can pipe input and capture the console: `printf 'cmd\r' | ./build/emulator --console-stdio ... rom.bin`
* `--console-in FILE` / `--console-out FILE` — back the DUART console with files. `--console-out` is the **cleanest** capture: it isolates the DUART console in the file, while the separate bit-banged `DEBUG_OUT` soft-UART still goes to stdout (so it won't garble the captured console).
* `--headless` — don't open the SDL window.  With `--screenshot` the full ENGINE -> PIXELS/VIDCMD FIFO -> PIXEL -> COMPOSITOR display-list pipeline still runs and fills the framebuffer; without it those models are skipped (guest timing is identical either way, because nothing the guest can read depends on them).
* `--selftest-displaylist` — no firmware needed: pokes a hand-built descriptor list into RAM, arms ENGINE, re-arms it on each frame IRQ, and at exit verifies the rendered frame is left-half red / right-half blue. Exercises arm, per-scanline `wait_hblank` release, VIDCMD deposits, eager SETs, RUN playback and `stop_after`. Exits nonzero on mismatch. Combine with `--screenshot`.
* `--screenshot FILE` — write the framebuffer to `FILE` (BMP) on exit. Convert to PNG to view: `sips -s format png FILE.bmp --out FILE.png`.
* `--serialb-pty` — expose DUART channel B on a host pty (path printed as "Serial B PTY: …"); `--serialb-in FILE` / `--serialb-out FILE` back channel B with files instead. Default: channel B unconnected. Used for PPP testing: run host `pppd` on the B pty against the guest's `/dev/ttyS1`. Channel B input EOF does not end the run (only the channel A console does).
* `--ps2-in FILE` — inject scripted PS/2 keystrokes (no SDL needed). Directives, one per line: `# comment`, `delay MS` (emulated ms), `text STRING` (`\r`/`\n`/`\t`/`\e`/`\\` escapes; shifted chars wrap LShift automatically), `raw HH HH..` (raw set-2 bytes, e.g. `raw AA` to fake a BAT). Bytes ride the normal 1 ms/byte pacing. Whatever guest is live consumes them, so `delay` past boot stages you don't target. The device model also answers host commands (reset/GETID/LEDs/typematic/enable), so guests may fully initialize the keyboard.
* `--mouse-in FILE` — inject scripted PS/2 mouse input into the PORTS mouse channel. Directives, one per line: `# comment`, `delay MS`, `move DX DY` (PS/2 sign: +Y is up), `button LMR` (or any subset; `none` releases all), `click L` (press, hold 50 ms, release), `raw HH HH..` (raw device→host bytes). Movement only becomes packets once the guest enables data reporting, exactly as on hardware, so `delay` past mouse init.
* `--joystick-in FILE` — inject scripted joystick/paddle state into PORTS. Directives: `# comment`, `delay MS`, `stick1 NAMES`, `stick2 NAMES` (space-separated `up down left right fire pin9 pin5`, or `none` to release), `paddle-a N` / `paddle-b N` (knob position 0..255, in line-tick counts). Interactive joystick input uses SDL gamepads, not host keys (host keys all go to the PS/2 keyboard).
* `--run-cycles N` — stop after N emulated SYSCLK cycles. This is the reliable way to make a run terminate; output is byte-for-byte deterministic across runs at the same N.
* `--no-throttle` — skip real-time pacing so a bounded run finishes fast.

Typical unattended invocation from `emulator/`:
```bash
printf 'help\r' | ./build/emulator --headless --console-stdio \
    --no-throttle --run-cycles 100000000 ../firmware/rom.bin
```
With no automation flags the emulator behaves exactly as before (pty + SDL window, runs until QUIT/`~.`).
