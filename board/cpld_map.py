#!/usr/bin/env python3
"""Schematic-side map of the six ATF1508 CPLDs: the single source of truth for
which symbol, sheet and Verilog each chip uses, and for the pins the Verilog
//PIN: block does not describe.

Plain data only; label_cpld.py (--chip) and check_cpld_pins.py read it.

Per chip:
  ref       symbol Reference on the sheet
  sheet     schematic file name (relative to board/)
  verilog   Verilog source with the //PIN: block (relative to board/)
  renames   Verilog port or //PIN: name -> KiCad net name; for a vector port
            the rename applies to the base (A_hi -> A gives A18..A23)
  jtag      pin -> net for the JTAG pins (14 TDI, 23 TMS, 62 TCK, 71 TDO)
  fixed     pin -> (net, shape) for non-port pins that still get a global label
  spare     pins that get a LOCAL label <CHIP>_SPARE_<pin>
  leave     pins the labeler and checker ignore (hand-placed parts sit there)

Every other I/O pin not named by the Verilog gets a no-connect flag.
"""

# ATF1508AS PLCC84 power pins (CPLD_Microchip:ATF1508ASx-xxJx84).  In the lib
# symbol only 3, 13 (VCC) and 7 (GND) are visible power_in pins; the rest are
# hidden passive pins stacked on them.
VCC_PINS = [3, 13, 26, 38, 43, 53, 66, 78]
GND_PINS = [7, 19, 32, 42, 47, 59, 72, 82]
POWER_PINS = {**{p: '+5V' for p in VCC_PINS}, **{p: 'GND' for p in GND_PINS}}
IO_PINS = [p for p in range(1, 85) if p not in POWER_PINS]

# JTAG pins of the ATF1508 PLCC84 and the label shape each gets.
JTAG_SHAPES = {14: 'input', 23: 'input', 62: 'input', 71: 'output'}
JTAG_PIN_NAMES = {'TDI': 14, 'TMS': 23, 'TCK': 62, 'TDO': 71}

CHIPS = {
    'GLUE': dict(
        ref='U13',
        sheet='glue.kicad_sch',
        verilog='../cpld/glue/glue.v',
        renames={
            'A_hi': 'A',
            'A_lo': 'A',
            'D': 'D_IO',                 # behind the 74AHCT245 pair (glue sheet)
            'nRAM_1_SEL': '~{RAM_SELECT_1}',
            'nRAM_2_SEL': '~{RAM_SELECT_2}',
            'nRAM_3_SEL': '~{RAM_SELECT_3}',
            'nRAM_4_SEL': '~{RAM_SELECT_4}',
            'nVSYNC': '~{VSYNC_IRQ}',
            'nR_W': '~{R}{slash}W',
        },
        jtag={14: 'JTAG_TDI', 23: 'JTAG_TMS', 62: 'JTAG_TCK', 71: 'GLUE_TO_PORTS_TD'},
        fixed={},
        spare=[],
        leave=[2],
    ),
    'PORTS': dict(
        ref='U14',
        sheet='ports.kicad_sch',
        verilog='../cpld/ports/ports.v',
        renames={'D': 'D_IO'},        # behind the 74AHCT245 pair (glue sheet)
        jtag={14: 'GLUE_TO_PORTS_TD', 23: 'JTAG_TMS', 62: 'JTAG_TCK', 71: 'PORTS_TO_AUDIO_TD'},
        fixed={},
        spare=[34, 41],
        leave=[],
    ),
    'AUDIO': dict(
        ref='U28',
        sheet='audio.kicad_sch',
        verilog='../cpld/audio/audio.v',
        renames={'CLK': 'PIXEL_CLK'},
        jtag={14: 'PORTS_TO_AUDIO_TD', 23: 'JTAG_TMS', 62: 'JTAG_TCK', 71: 'AUDIO_TO_ENGINE_TD'},
        fixed={},
        spare=[],
        leave=[],
    ),
    'ENGINE': dict(
        ref='U18',
        sheet='engine.kicad_sch',
        sheet_fallback='expansion.kicad_sch',
        verilog='../cpld/engine/engine.v',
        renames={},
        jtag={14: 'AUDIO_TO_ENGINE_TD', 23: 'JTAG_TMS', 62: 'JTAG_TCK', 71: 'ENGINE_TO_PIXEL_TD'},
        fixed={},
        spare=[],
        leave=[],
    ),
    'PIXEL': dict(
        ref='U19',
        sheet='pixel.kicad_sch',
        verilog='../cpld/pixel/pixel.v',
        renames={},
        jtag={14: 'ENGINE_TO_PIXEL_TD', 23: 'JTAG_TMS', 62: 'JTAG_TCK', 71: 'PIXEL_TO_COMPOSITOR_TD'},
        fixed={
            2: ('CLK2', 'input'),
            67: ('DUART_OP4', 'input'),
            68: ('DUART_OP5', 'input'),
            69: ('DUART_OP6', 'input'),
            70: ('DUART_OP7', 'input'),
        },
        spare=[30, 34, 37, 50, 54, 55, 63, 64, 65, 73, 74, 75, 76, 77, 79, 80, 81],
        leave=[],
    ),
    'COMPOSITOR': dict(
        ref='U22',
        sheet='compositor.kicad_sch',
        verilog='../cpld/compositor/compositor.v',
        renames={},
        jtag={14: 'PIXEL_TO_COMPOSITOR_TD', 23: 'JTAG_TMS', 62: 'JTAG_TCK', 71: 'JTAG_TDO'},
        fixed={},
        spare=[],
        leave=[],
    ),
}
