# Griffin Project

## Intent

* This is a homebrew 68000 computer intended to let me use some parts I've had for decades in my bin, including 68000P12, 2x KM681000BLP-7, a bunch of 70ns 27C512, and a handful of discrete parts.  The IC parts bin inventory is at chip-inventory.csv but I also have assorted LEDs, transistors, diodes, and a large selection of capacitors and resistors.

* Goals include but do not require and are not limited to, in rough increasing order of difficulty:
  * FUZIX
  * Running System 3 binaries without memory protection or paging
  * CP/M-68K
  * Display bitmaps of various resolutions, starting with 720*480*1bpp in NTSC up to 640*480*8bpp VGA
  * MiNT variants and EmuTOS

## Definition

* The project is defined by griffin.md and griffin.yml and the KiCAD board design follows closely but is slightly less authoritative.
  * KICAD board design is in board/ but has progressed significantly from rev 1.
    * A print of the PCB and schematic of Rev 1 are in board_rev_1_pcb.pdf and board_rev_1_schematic.pdf
    * The PCB Rev 1 netlist is in board/board-pcb-rev-1.distilled.txt except with bodges applied as noted in griffin.yml, produced with kicad_netlist_summary_2.py
    * Rev 1 gerbers are in board/board-gerb
  * GLUE Verilog for ATF1508AS in cpld/glue
  * VIDEO Verilog for ATF1508AS in cpld/video
  * A third ATF1508, "ENGINE", is for reading video memory which will then be snooped by VIDEO and maybe reading and writing audio data if there are logic cells left over.
  * Makefile for both GLUE and VIDEO in cpld
  * 68000 ROM in firmware/{crt0.s,linker.ld,rom.cpp,Makefile} and associated other files in firmware
  * 68000 bringup ROM in sanity/{sanity.s,linker.ld,Makefile}
  * There is currently IO_MCU firmware in io-mcu/{Makefile,main.c} and generated Python script to flash over SPI in io-mcu/at89s52_isp.py, but the IO_MCU has turned out to be a problem for PCB Rev 1; ignore it for now.
  * emulator in emulator/ and the intent is to at least emulate the 68000 and MMIO accesses.  TBD whether to emulate the ATF1508's using Verilator.

* When possible, store new hardware definitions in griffin.yml; register addresses, bits and bitfields, constants, protocol between peripherals, constants, and then generate included headers.

* Keep in mind for instruction-counted loops that there are ROM wait states.

* The pins have been hand-assigned to ATF1508 pins, and those must remain where assigned because a PCB has already been manufactured.

* I don't have "timeout", use a perl one-liner instead.

## Building components

* Generate C++, Verilog, and assembly includes for components with `make` at project root.
* Build glue and video in cpld/ with `make glue` or `make video` or `make` (for both)
* Configure emulator CMake in cpld/ with `cmake -Bbuild .` and build with `cmake --build build`
* Build the ROM in firmware/ with `make`.  The toolchain is made from a Docker image of an Ubuntu 24 build of crosstool-ng for m68k-unknown-elf for 68000 and not for 68832; see firmware/m68k-crosstool-ng.config, firmware/m68k-{g++,gcc,objcopy,objdump}, BUILD_TOOLCHAIN_CONTAINER, Dockerfile.  The toolchain .tar.gz might not be in git.
* Build the sanity test ROM image in firmware/ with `make`.  Same toolchain as firmware/.  (Probably should unify the toolchains between firmware and rom at some point...)
* Build the IO MCU firmware in io-mcu/ with `make`, requires `sdcc`.

## Testing components

* sanity/sanity.bin and firmware/rom.bin should execute in emulator/emulator/build/emulator.
  * note that IO-MCU UART TX and RX is through a PTY.

## General design guidelines

* Prefer correction by construction, but not through unreasonable complexity.

## Claude Code Guidelines

* Prefer C++20 for host-based tools and bare-metal-capable C++20 for the 68000 ROM firmware.

### Design

* In general prefer facilities don't cross-communicate except absolutely necessary.  E.g. a CF card facility can fill in a string with identity, but wouldn't call the UART to print it.  A higher function would call to get the identity, and then call whatever routine it prefers to print the identity or store it in NVRAM or whatever.

### C/C++ Style

- All block statements (`if`, `else`, `for`, `while`, `switch`) must use braces, even for single-line bodies.
- Opening braces go on their own line for block statements and function definitions with the exception of empty blocks, e.g. "while(blah) {}" 
- It's not necessary to translate C style to the style of Verilog "begin" and "end" , as Verilog's "begin" and "end" are words and don't key visually the same way.

```c
if (condition)
{
    do_thing();
}
else
{
    do_other_thing();
}

void foo(void)
{
    bar();
}
```

### Verilog Style

- Prefer computation over block conditionals for simple constructs with just a couple of terms.  E.g. for this:
```
           if (in_vsync)
               nCPST_SYNC <= 1'b0;
           else
               nCPST_SYNC <= ~in_hsync;
```
prefer this:
```
           nCPST_SYNC <= ~(in_vsync | in_hsync);
```

#### Module Declaration Style

- Parameters go on a separate `#(parameter ...)` line between the module name and the port list. Each is on its own line when there are several:
  ```verilog
  module ShaderCore
      #(parameter WORD_WIDTH=32, ADDRESS_WIDTH=16, SDRAM_ADDRESS_WIDTH=24)
  (
      input wire clock,
      ...
  );
  ```
- Ports always have explicit `wire`/`reg` type annotations even when `wire` is the default.
- Inputs are `input wire`, outputs are `output wire` for combinational or `output reg` for stateful.

#### Naming Conventions

- Module names are **PascalCase**: `ShaderCore`, `RegisterFile`, `BlockRam`, `RISCVDecode`, `GPU32BitInterface`.
- Instance names are **camelCase**: `instDecode`, `instRam`, `dataRam`, `shaderCore`
- Signals/wires/regs are **snake_case**: `rs1_value`, `alu_result`, `data_ram_write`, `decode_opcode_is_branch`.
- `localparam` constants are **UPPER_SNAKE_CASE**: `ALU_OP_ADD`, `STATE_FETCH`, `FP_MULTIPLY_LATENCY`.

#### Structural Patterns

Large selection logic is written as chained ternary expressions rather than `case` statements when the selection is among wires (combinational):
```verilog
assign alu_op1 =
    (decode_opcode_is_ALU_reg_imm || ...) ? $signed(rs1_value) :
    decode_opcode_is_lui ? $signed(decode_imm_upper) :
    decode_opcode_is_jal ? $signed(PC) :
    $signed(32'hdeadbeef);
```
The final "else" case is typically a recognizable hex sentinel (`32'hdeadbeef`, `32'hcafebabe`, `32'hdeafca75`) rather than `x` or `0`, making simulation debugging easier.

##### Case statements for sequential/registered logic
`case` is used inside `always @(posedge clock)` blocks for state machine transitions and the ALU. Every `case` has a `default` branch. Each branch uses `begin`/`end` even for single statements.

##### State machines are linear `localparam` enumerations
States are numbered `localparam` integers (`5'd00`, `5'd01`, ...) with descriptive names. The state reg width is chosen to fit. State numbering has gaps (e.g., `STATE_RETIRE = 5'd06`, `STATE_FP_WAIT = 5'd16`) -- likely from states being added/removed over time.

#### Idioms

##### `/* verilator public */` annotations
Liberally applied to signals and memories that need to be visible from the C++ testbench. Used on `reg`, `wire`, and memory array declarations alike.

##### Verilator lint pragmas as inline comments
Unused signals are wrapped in lint-off/lint-on pairs:
```verilog
/* verilator lint_off UNUSED */
wire [4:0] decode_rs3 /* verilator public */;
/* verilator lint_on UNUSED */
```
Similarly `PINCONNECTEMPTY` for unconnected outputs of submodule instances.

##### Brace-comment annotations for deeply nested blocks
In `GPU32BitInterface.v`, `begin`/`end` and `case`/`endcase` get `// {` and `// }` comments to visually match nesting levels in long procedural blocks:
```verilog
always @(posedge clock) begin // {
    if(!reset_n) begin // {
        ...
    end else begin // } {
        ...
    end // }
end // }
```
This appears in the more complex state machines but not in simpler modules.

##### Sentinel hex values as "should never happen" markers
Default/fallthrough cases produce recognizable hex patterns:
- `32'hdeadbeef` -- default ALU operand 1
- `32'hcafebabe` -- default ALU operand 2
- `32'hdeafca75` -- default ALU result
- `32'hdeadbee1`, `32'hdeadbee2`, `32'hdeadbee3` -- unimplemented register read paths

These are clearly chosen to be grep-able and recognizable in waveform viewers.

##### Combinational `casez` for priority encoding
`int_to_float.v` uses `casez` with wildcard `?` bits to find the highest set bit, effectively a priority encoder written out explicitly as 31 pattern-match lines. No loop or generate -- the full expansion is visible and unambiguous.

##### Reduction operators for special-value detection
Float classification uses reduction operators idiomatically:
```verilog
wire is_zero = !(|op);          // NOR reduction
wire exp_all_on = &exp_part;    // AND reduction
```

##### Explicit sign handling with `$signed` / `$unsigned`
Signedness is always called out at point of use rather than relying on port declarations:
```verilog
result <= $signed(operand1) + $signed(operand2);
result <= $signed(operand1) >>> operand2;
PC <= $unsigned({alu_result[WORD_WIDTH-1:1],1'b0});
```

#### Design Philosophy

### Flat, explicit, and readable over clever
- No `generate` blocks anywhere. Multi-port is done by instantiating twice.
- No `for` loops in synthesizable code.
- Bit-field decoding is written out as individual `assign` statements rather than packed into functions or tasks.
- The full casez expansion in `int_to_float.v` (31 lines) is preferred over a parameterized loop.

### Software-engineer sensibility
- Comments explain *why*, not *what* (e.g., "Old data read-during-write behavior, as recommended by Altera" rather than "reads memory").
- TODO/XXX comments are left in as honest markers of known limitations.
- The `// {` / `// }` nesting hints borrow from C brace-matching habits.
- Naming is descriptive and long rather than terse (`decode_opcode_is_ALU_reg_imm` over `op_ari`).

### Pragmatic over pure
- State numbering has gaps -- states were added/removed without renumbering.
