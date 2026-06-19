# Video FIFO Bringup Wiring Plan

Two IDT7200L 256x9 FIFOs on solderless breadboard.
EVEN FIFO carries D[15:8] (upper byte), ODD carries D[7:0] (lower byte).
Verify all 7200 pin numbers against datasheet before wiring.

## 1. Power and reset (both 7200s)

- Vcc, GND — 100nF bypass cap each
- /RS — wire together, tie to system nRESET (active-low)
- /XI — tie to Vcc (no expansion)
- FL/RT — tie to Vcc (FIFO mode, not LIFO)

## 2. Write side (data bus to 7200 inputs)

EVEN 7200 D inputs:

| 7200 signal | CPU bus signal |
|-------------|----------------|
| D0          | D8             |
| D1          | D9             |
| D2          | D10            |
| D3          | D11            |
| D4          | D12            |
| D5          | D13            |
| D6          | D14            |
| D7          | D15            |
| D8          | q8_toggle (from ENGINE, see below) |

ODD 7200 D inputs:

| 7200 signal | CPU bus signal |
|-------------|----------------|
| D0          | D0             |
| D1          | D1             |
| D2          | D2             |
| D3          | D3             |
| D4          | D4             |
| D5          | D5             |
| D6          | D6             |
| D7          | D7             |
| D8          | q8_toggle (same signal as EVEN) |

Both 7200s /W — wire together, driven by ENGINE nFIFO_W output.

## 3. Read side (7200 outputs to VIDEO CPLD)

Q outputs are bused together (only one /R asserted at a time):

| 7200 Q signal | VIDEO CPLD pin | VIDEO signal |
|---------------|----------------|--------------|
| Q0            | 36             | FIFO_Q[0]    |
| Q1            | 31             | FIFO_Q[1]    |
| Q2            | 30             | FIFO_Q[2]    |
| Q3            | 28             | FIFO_Q[3]    |
| Q4            | 37             | FIFO_Q[4]    |
| Q5            | 39             | FIFO_Q[5]    |
| Q6            | 44             | FIFO_Q[6]    |
| Q7            | 9              | FIFO_Q[7]    |
| Q8            | 45             | FIFO_Q8      |

Read strobes:

| Signal         | VIDEO CPLD pin | 7200 target     |
|----------------|----------------|------------------|
| nFIFO_RE_EVEN  | 50             | EVEN 7200 /R     |
| nFIFO_RE_ODD   | 52             | ODD 7200 /R      |

## 4. ENGINE CPLD — generate /W and toggle bit

ENGINE inputs already wired on PCB:
- nENGINE_SELECT: pin 84 (GLUE asserts for 0xD0xxxx)
- R_nW: pin 25
- nLDS: pin 64

ENGINE outputs (bodge wire to breadboard):
- nFIFO_W: ENGINE pin 10 — wire to both 7200 /W
- q8_toggle: ENGINE pin 8 — wire to both 7200 D8

ENGINE input (bodge wire from breadboard):
- nFIFO_HF: ENGINE pin 6 — wire to either 7200 /HF (both fill at same rate)

Minimal ENGINE Verilog:

```verilog
input  wire  nENGINE_SELECT,  // pin 84
input  wire  R_nW,            // pin 25
input  wire  nLDS,            // pin 64
input  wire  nFIFO_HF,        // pin 6  — either 7200 half-full (active low)
output wire  nFIFO_W,         // pin 10 — bodge to both 7200 /W
output wire  q8_toggle_out,   // pin 8  — bodge to both 7200 D8

wire cpu_write = ~nENGINE_SELECT & ~R_nW & ~nLDS;
assign nFIFO_W = ~cpu_write;

reg q8_toggle = 0;
always @(posedge CPUCLK or posedge RESET)
    if (RESET)
        q8_toggle <= 0;
    else if (cpu_write)
        q8_toggle <= ~q8_toggle;

assign q8_toggle_out = q8_toggle;
```

## 5. Signals NOT needed for bringup

- EF, FF — leave unconnected (or wire to LED for debug)
- /XO — leave unconnected

## 6. Wiring checklist

```
[ ] Both 7200s placed, powered, bypassed
[ ] /RS tied to nRESET, /XI to Vcc, FL/RT to Vcc
[ ] EVEN D0-D7 wired to CPU D8-D15
[ ] ODD D0-D7 wired to CPU D0-D7
[ ] Both D8 wired to ENGINE pin 8 (q8_toggle)
[ ] Both /W wired to ENGINE pin 10 (nFIFO_W)
[ ] Either /HF wired to ENGINE pin 6 (nFIFO_HF)
[ ] Both Q0-Q8 bused together
[ ] Q0-Q8 wired to VIDEO CPLD pins 36,31,30,28,37,39,44,9,45
[ ] EVEN /R wired to VIDEO pin 50 (nFIFO_RE_EVEN)
[ ] ODD /R wired to VIDEO pin 52 (nFIFO_RE_ODD)
[ ] ENGINE reflashed with /W + toggle stub
```

## 7. Firmware test loop

Write 40 words per scanline to 0xD00000, paced by LINE_TOGGLE.

```
wait:
    move.b  VIDEO_STATUS, d1      | read v_cnt[0]
    andi.b  #1, d1
    cmp.b   d2, d1                | d2 = last toggle
    beq.s   wait
    move.b  d1, d2

    | burst 40 words — preload regs with pattern
    movem.l d3-d7/a2-a6, (a0)    | 10 regs = 20 longwords = 40 words
    bra.s   wait
```

Pre-load d3-d7/a2-a6 with e.g. 0xAA55AA55 for checkerboard.
a0 = 0xD00000 (ENGINE write address, doesn't auto-increment for FIFO).

Timing: MOVEM.L 10 regs = 8 + 80 = 88 cycles (but 68000 MOVEM.L
writes 2 words per reg = 20 bus cycles; each ~8 clocks with 0WS ENGINE
DTACK). Total ~176 SYSCLK. Budget is ~445 SYSCLK/line. Comfortable.

## 8. Expected result

- Before VIDEO_CTRL enable: black screen (background_color)
- After enable: 640x480 static pattern from CPU-written FIFO data
- Checkerboard 0xAA55 = alternating fg/bg pixels in 8-pixel groups
- FIFO_ERROR flag stays clear if CPU keeps up with scanout
