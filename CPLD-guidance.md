# ATF1508AS CPLD Guidance

Lessons learned from Griffin GLUE optimization experiments and fitter analysis.  Applies to GLUE, VIDEO, and ENGINE designs targeting the ATF1508AS (128 macrocells, 8 LABs of 16, ~40 fan-in per LAB, 5 product terms per macrocell).

## The fitter is unpredictable

The ATF1508 fitter (fit1508.exe) solves a constrained bin-packing problem: it must place every signal into a macrocell while respecting per-LAB fan-in limits (40 inputs), product-term limits (5 PTs per cell, cascadable), and pin assignments.  Small Verilog changes can cause large swings in macrocell count, foldback count, and product-term usage.

Experimental results from GLUE (all with identical functionality):

| Change                                       | MCs   | Foldbacks | Outcome         |
|----------------------------------------------|-------|-----------|-----------------|
| Baseline (4-rate systick case)               | 104   | —         | Starting point  |
| Simplify to 2-rate systick, add IRQ enable   | 98    | 18        | **Best so far** |
| Direct 8-bit reload from data bus            | 111   | —         | Worse           |
| Merge SYSTICK_CONFIG into CONFIG register    | 110   | —         | Worse           |
| Move systick_irq_enable to SYSTICK_CONFIG    | 104   | 27        | Worse           |

Key takeaways:

- **"Simpler logic" does not reliably mean fewer macrocells.**  The direct-reload approach looked simpler in Verilog but cost more because each reload bit needed the full address decode in its product terms.

- **Merging registers can go either direction.**  Combining SYSTICK_CONFIG into CONFIG forced the fitter to route signals across distant LABs, increasing foldbacks.  Splitting them back out restored the lower count.

- **"Group by dataflow" is not a reliable rule.**  Moving systick_irq_enable to the SYSTICK_CONFIG write (where the rate bit lives) made things worse (104 cells, 27 foldbacks vs. 98/18), because the fitter had already found a good placement with it in CONFIG.

- **The only reliable test is running the fitter.**  Intuition about what "should" be cheaper is wrong often enough that every change must be verified.  Keep the old `.fit` output for comparison.

## What foldbacks are and why they matter

When a combinational node's output is needed in a LAB other than where it's placed, the fitter creates a **foldback**: a copy of that node in the consuming LAB.  Each foldback consumes one macrocell.

In the GLUE baseline (98/128 MCs), there are 18 foldbacks.  The worst offender is the systick tick gate (`abc:n260`), replicated in 4 LABs because it feeds all systick subdivider and counter registers.  The timer prescaler zero-detect and DTACK gate are each replicated in 3 LABs.

Foldbacks are not inherently bad — they're the fitter's way of solving fan-in constraints — but they consume macrocells.  A signal replicated in 4 LABs costs 4 MCs for what is logically one node.

## Shared vs. separate prescaler

The GLUE design shares a 3-bit prescaler between the systick timer chain and the TIMER (arm/stall) facility.  This saves 3 flip-flops but forces the prescaler-expired gate to fan out across many LABs, causing foldback replication.

Splitting into two independent prescalers would add ~3 FFs but might reduce foldbacks if each chain's signals can stay in fewer LABs.  Whether this actually helps is unknowable without running the fitter.

## What the fitter report tells you

`cpld/fit_report.py` parses the `.fit` and `.edif` files to produce a readable report with Verilog signal names and heuristic annotations for ABC optimizer nodes.  It is useful for:

1. **Headroom assessment.**  Before adding a feature, check which LABs have spare macrocells and fan-in budget.  LABs at 35+/40 fan-in are near capacity.

2. **Spotting genuine waste.**  A 20-PT equation or a signal replicated in 4+ LABs is worth investigating.  A 2-PT equation replicated once is not.

3. **Post-hoc validation.**  After adding logic, compare the new report against the previous one.  If foldbacks jumped significantly or a LAB hit 40/40 fan-in, the new logic is fighting the fitter and may benefit from restructuring.

4. **Understanding ABC node names.**  The annotations (`systick tick gate`, `GLUE CONFIG WR decode`, etc.) tell you what each optimizer node actually does, making the equations readable.

The report does **not** tell you what change to make.  It tells you where the pressure is, so you can make an informed guess before running the fitter to verify.

## Current GLUE headroom (as of 98/128)

| LAB | Used/16 | Fan-in | Notes                                    |
|-----|---------|--------|------------------------------------------|
| A   | 15      | 30/40  | RAM/ROM selects, DTACK gen, GLUE decode  |
| B   | 21*     | 34/40  | Systick subdiv+counter, 4 foldbacks      |
| C   | 23*     | 35/40  | Timer cnt/period, DTACK pin, 3 foldbacks |
| D   | 16      | 35/40  | Systick counter, 3 foldbacks             |
| E   | 19*     | 35/40  | IPL/BERR, timer period, 3 foldbacks      |
| F   | 23*     | 35/40  | Addr decode, prescaler, wait-state       |
| G   | 10      | 29/40  | Data bus readback, debug, 1 foldback     |
| H   | 10      | 18/40  | Peripheral selects, 1 foldback           |

*includes foldbacks; "used" counts pins + feedback + foldback nodes.

LABs G and H have the most room.  LABs B–F are near fan-in limits.
New features that need many internal signals (like UART RX state
machines) will likely push the fitter to spread across LABs, creating
more foldbacks.

## Practical strategy for adding features

1. **Don't pre-optimize existing logic to "make room."**  At 76% utilization the current design works.  Trying variations to shave off a few MCs risks wasting time on a problem the fitter may solve differently once the new logic is present.

2. **Add the feature, then assess.**  Implement UART RX (or PS/2, or whatever is next), run the fitter, and check the report.  If it fits, move on.

3. **If it doesn't fit, use the report to triage.**  Look for:
   - Signals with 4+ foldback copies (candidates for restructuring)
   - LABs at 40/40 fan-in (the fitter literally cannot route more
     signals there)
   - Equations with 15+ product terms (may benefit from factoring)

4. **Try one structural change at a time and verify.**  Don't combine "split the prescaler" with "refactor DTACK" in one attempt — you won't know which helped (or hurt).

5. **Use `-preassign keep`** when running the fitter, not `-preassign try`.  The `try` mode silently reassigns pins, which will not match the manufactured PCB.

## Running the fitter report

```
cd cpld
python3 fit_report.py glue/glue.fit glue/glue.edif
python3 fit_report.py video/video.fit video/video.edif   # when available
```

The report sections are: Utilization Summary, Per-LAB Placement, Foldback Analysis (with consumer tracking), Fan-In Detail, Product Term Usage, and All Equations.  ABC optimizer nodes are annotated with heuristic labels like `GLUE CONFIG WR decode` or `systick tick gate`.

# Some notable failures of intuition

* Using `>=` uses **fewer** macrocells and logic than `>`, maybe because there are multiple bits that are "don't care".

* **One-hot state encoding does not save macrocells on ATF1508.** Tested on ENGINE (6-state DMA controller): changing from 3-bit binary encoding (`3'd0`..`3'd5`) to 6-bit one-hot (`6'b000001`..`6'b100000`) did not reduce utilization — the fitter came out at 96% vs. 97%.  The intuition was sound: each `state == X` comparison becomes a single-bit test, eliminating multi-bit decode product terms.  But the cost of 6 flip-flops (vs. 3) and wider fan-out from 6 state bits feeding every transition term offset the savings.  The fitter's bin-packing is good enough at optimizing 3-bit comparisons that the one-hot structure doesn't help it.  (Note: an initial test appeared to show 81% — but this was caused by `3'd8`/`3'd16`/`3'd32` literals silently truncating to 0, collapsing three states and producing a broken but smaller design.)
