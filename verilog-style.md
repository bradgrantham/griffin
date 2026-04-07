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

**ATF1508 note:** One-hot state encoding was tested on ENGINE and did not save macrocells vs. binary encoding (96% vs. 97%). Use binary encoding for ATF1508 state machines. See CPLD-guidance.md for details.

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
