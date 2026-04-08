# ATF1508AS CPLD Guidance

Lessons from Griffin GLUE/VIDEO/ENGINE designs (128 MC, 8 LABs×16, ~40 fan-in/LAB, 5 PT/MC).

## Key Lessons

- **Avoid constants that are mostly 1s.** Subtraction-by-1 expands to all-ones addition—expensive. Flip counters and detect wrap at all-1s instead of 0.
- **Simpler Verilog ≠ fewer macrocells.** Direct-reload looked simpler but cost more due to full address decode per reload bit.
- **Register merging can go either direction.** Combining SYSTICK_CONFIG into CONFIG forced cross-LAB routing, increasing foldbacks. Splitting restored lower count.
- **"Group by dataflow" is unreliable.** Moving systick_irq_enable near the rate bit worsened results (104 MC/27 foldbacks vs. 98/18).
- **The only reliable test is running the fitter.** Verify every change against the previous `.fit` output.
- **`>=` uses fewer MCs than `>`.** Don't-care bits help.
- **One-hot state encoding does not save MCs on ATF1508.** Tested on ENGINE (6-state DMA): one-hot vs. 3-bit binary came out 96% vs. 97%—extra flip-flops and fan-out offset the decode savings.

## Foldbacks

A foldback is a copy of a combinational node placed in a consuming LAB to satisfy fan-in constraints. Each costs one MC. Common offenders: systick tick gate (4 LABs), timer prescaler zero-detect and DTACK gate (3 LABs each). Not inherently bad, but they consume MCs.

## Fitter Report Tool

`cpld/fit_report.py glue/glue.fit glue/glue.edif` produces: Utilization Summary, Per-LAB Placement, Foldback Analysis, Fan-In Detail, Product Term Usage, All Equations. ABC nodes are annotated with heuristic labels.

Use it for: headroom assessment before adding features, spotting waste (20-PT equations, 4+ foldback copies), post-change validation.

## Strategy for Adding Features

1. Don't pre-optimize to "make room"—76% utilization works; premature tweaks risk regressions.
2. Add the feature, run the fitter, check the report.
3. If it doesn't fit, triage: signals with 4+ foldbacks, LABs at 40/40 fan-in, equations with 15+ PTs.
4. One structural change at a time—don't combine changes.
5. Use `-preassign keep`, not `-preassign try` (try silently reassigns pins, won't match PCB).
