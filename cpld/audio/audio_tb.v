// audio_tb.v — iverilog testbench for the AUDIO chip (audio.v)
//
// Self-checking.  Every named check prints PASS or FAIL; the run ends with
// "TESTBENCH RESULT: PASS" (and exit status 0) only if no check failed.
//
// ---------------------------------------------------------------------------
// What this bench is
// ---------------------------------------------------------------------------
//
// The chip's contract is stated in audio/audio_chip.h (the host model, the
// executable spec) and restated here as three kinds of check:
//
//   1. THE POP SCHEDULE, against the model.  audio/tb_vectors.cpp runs a random
//      stimulus (control records, sample bursts, channels run dry, a FIFO
//      overfilled) through the model and writes tb/stim.hex, tb/pops.hex and
//      tb/regs.hex.  The bench deposits the same bytes into behavioural 7200s
//      at the same instants (right after the edge that starts each 800-clock
//      line) and requires nSAMPLE_R[c] to pulse for exactly one clock after
//      the counter edge of every step the model popped, and never otherwise,
//      and the PERIOD/VOLUME registers to hold the model's values after the
//      edge that ends every line.
//
//   2. THE GATE LAW, closed form on every clock.  GATE is a register, so its
//      value after edge m is a function of the state during the clock before:
//          GATE[c](m) = on(VOLUME[c], frame) & ~empty[c] & ~nSAMPLE_R[c](m)
//      with frame = timebase[8:3] before the edge, timebase = m - 1 mod 512
//      (the chip's counter, counted here independently from reset), on() the
//      six rate-multiplier windows of audio_chip.h, empty from the bench's
//      FIFO, and the pop term equal to the pin after the edge.  VOLUME is
//      read from the chip's register (its value is checked against the model
//      by check 1), so the law itself is what is under test.
//
//   3. THE FIFO PROTOCOL.  Through reset both /R outputs are high (the 7200
//      needs /R inactive while /RS is low); a sample /R pulse is exactly one
//      clock; the control reader holds /R low for exactly two clocks and
//      never reads an empty FIFO (the behavioural FIFO counts it as an error).
//
// Time.  m counts clock edges from the last edge at which nRESET was low
// (m = 0); after edge m the chip's timebase is m mod 512, line k starts at
// edge 800 k (800 = 50 x 16, so lines are step-aligned), and step s's counter
// update is edge 16 (s + 1).  Deposits for line k are made #1 after edge
// 800 k, which is when ENGINE's word would land in the model's ordering
// (deposits, then the 16-clock reader window, then the counter update).
//
// ---------------------------------------------------------------------------
// Negative controls
// ---------------------------------------------------------------------------
//
// `make audio-sim` also compiles this bench with -DMUTATE=1..5, each of which
// perturbs one expectation, and requires every such run to FAIL:
//
//   MUTATE 1  pops expected one step late
//   MUTATE 2  window 4 taken as tick mod 4 == 1
//   MUTATE 3  the gate law ignores the empty flag
//   MUTATE 4  registers expected one line late
//   MUTATE 5  a two-clock /R pulse accepted

`timescale 1ns / 1ps

// Behavioural IDT7200, byte wide, 256 deep.  Q is valid while /R is low and
// the FIFO has data; the pointer advances on /R's rising edge; a rising edge
// on an empty FIFO is an error the bench counts (the part ignores it, the
// chip must never do it on the control FIFO).
module Fifo7200Model
(
    input  wire       nR,
    output wire [7:0] Q,
    output wire       nEF,
    input  wire       nRESET
);
    reg [7:0] mem [0:255];
    integer head;
    integer tail;
    integer count;
    integer reads_while_empty;
    integer writes_while_full;

    assign nEF = (count != 0);
    assign Q   = (!nR && count != 0) ? mem[head] : 8'hzz;

    task write(input [7:0] value);
    begin
        if (count < 256)
        begin
            mem[tail] = value;
            tail = (tail + 1) % 256;
            count = count + 1;
        end
        else
        begin
            writes_while_full = writes_while_full + 1;
        end
    end
    endtask

    always @(posedge nR)
    begin
        if (nRESET)
        begin
            if (count != 0)
            begin
                head = (head + 1) % 256;
                count = count - 1;
            end
            else
            begin
                reads_while_empty = reads_while_empty + 1;
            end
        end
    end

    always @(negedge nRESET)
    begin
        head = 0;
        tail = 0;
        count = 0;
        reads_while_empty = 0;
        writes_while_full = 0;
    end

    initial
    begin
        head = 0;
        tail = 0;
        count = 0;
        reads_while_empty = 0;
        writes_while_full = 0;
    end
endmodule

module audio_tb;

`ifndef MUTATE
    `define MUTATE 0
`endif

    reg CLK = 0;
    reg nRESET = 0;

    wire       nCONTROL_EF;
    wire [7:0] CONTROL_Q;
    wire       nCONTROL_R;
    wire [3:0] nSAMPLE_EF;
    wire [3:0] nSAMPLE_R;
    wire [3:0] GATE;

    Audio dut
    (
        .CLK(CLK), .nRESET(nRESET),
        .nCONTROL_EF(nCONTROL_EF), .CONTROL_Q(CONTROL_Q), .nCONTROL_R(nCONTROL_R),
        .nSAMPLE_EF(nSAMPLE_EF), .nSAMPLE_R(nSAMPLE_R), .GATE(GATE)
    );

    Fifo7200Model control (.nR(nCONTROL_R), .Q(CONTROL_Q), .nEF(nCONTROL_EF), .nRESET(nRESET));
    wire [7:0] unused_q0, unused_q1, unused_q2, unused_q3;
    Fifo7200Model sample0 (.nR(nSAMPLE_R[0]), .Q(unused_q0), .nEF(nSAMPLE_EF[0]), .nRESET(nRESET));
    Fifo7200Model sample1 (.nR(nSAMPLE_R[1]), .Q(unused_q1), .nEF(nSAMPLE_EF[1]), .nRESET(nRESET));
    Fifo7200Model sample2 (.nR(nSAMPLE_R[2]), .Q(unused_q2), .nEF(nSAMPLE_EF[2]), .nRESET(nRESET));
    Fifo7200Model sample3 (.nR(nSAMPLE_R[3]), .Q(unused_q3), .nEF(nSAMPLE_EF[3]), .nRESET(nRESET));

    // ---- vectors -----------------------------------------------------------
    localparam MAX_EVENTS = 200000;
    localparam MAX_POPS   = 400000;
    localparam MAX_LINES  = 4096;
    reg [39:0] stim [0:MAX_EVENTS-1];
    reg [39:0] pops [0:MAX_POPS-1];
    reg [63:0] regs [0:MAX_LINES-1];
    integer n_events;
    integer n_pops;
    integer n_lines;
    integer next_event;
    integer next_pop;

    // ---- bookkeeping ------------------------------------------------------
    integer errors;
    integer m;                  // edges since the last reset edge
    integer line;
    integer c;
    integer i;
    integer pop_checks;
    integer reg_checks;
    integer gate_checks;
    integer pulse_checks;
    reg [3:0] expected_pop;     // channels the model popped at this step
    reg [3:0] prev_nsample_r;
    reg [3:0] obs_nsample_r;
    reg [8:0] frame_tick;
    reg [5:0] volume_pre [0:3];
    reg [3:0] empty_pre;
    reg [3:0] expected_gate;
    reg [3:0] obs_gate;
    reg       on_bit;
    integer   control_low_run;
    integer   step;

    function on_window;
        input [5:0] volume;
        input [5:0] t;
        reg w5, w4, w3, w2, w1, w0;
        begin
            w5 = t[0];
            w4 = (`MUTATE == 2) ? (t[1:0] == 2'd1) : (t[1:0] == 2'd2);
            w3 = (t[2:0] == 3'd4);
            w2 = (t[3:0] == 4'd8);
            w1 = (t[4:0] == 5'd16);
            w0 = (t == 6'd32);
            on_window = (volume[5] & w5) | (volume[4] & w4) | (volume[3] & w3) |
                        (volume[2] & w2) | (volume[1] & w1) | (volume[0] & w0);
        end
    endfunction

    task fail;
        input [511:0] what;
        begin
            errors = errors + 1;
            if (errors <= 25)
            begin
                $display("FAIL m=%0d line=%0d: %0s", m, line, what);
            end
        end
    endtask

    always #20 CLK = ~CLK;      // 25 MHz-ish; the period is irrelevant to the checks

    // Pre-edge samples for the gate law: what the chip saw during the clock.
    always @(negedge CLK)
    begin
        volume_pre[0] = dut.volume0;
        volume_pre[1] = dut.volume1;
        volume_pre[2] = dut.volume2;
        volume_pre[3] = dut.volume3;
        empty_pre = ~nSAMPLE_EF;
        prev_nsample_r = nSAMPLE_R;
    end

    initial
    begin
        errors = 0;
        pop_checks = 0;
        reg_checks = 0;
        gate_checks = 0;
        pulse_checks = 0;
        control_low_run = 0;
        n_events = 0;
        n_pops = 0;
        n_lines = 0;

        for (i = 0; i < MAX_EVENTS; i = i + 1) stim[i] = 40'hFFFFFFFFFF;
        for (i = 0; i < MAX_POPS; i = i + 1) pops[i] = 40'hFFFFFFFFFF;
        for (i = 0; i < MAX_LINES; i = i + 1) regs[i] = 64'hFFFFFFFFFFFFFFFF;
        $readmemh("tb/stim.hex", stim);
        $readmemh("tb/pops.hex", pops);
        $readmemh("tb/regs.hex", regs);
        while (n_events < MAX_EVENTS && stim[n_events] != 40'hFFFFFFFFFF) n_events = n_events + 1;
        while (n_pops < MAX_POPS && pops[n_pops] != 40'hFFFFFFFFFF) n_pops = n_pops + 1;
        while (n_lines < MAX_LINES && regs[n_lines] != 64'hFFFFFFFFFFFFFFFF) n_lines = n_lines + 1;
        $display("vectors: %0d events, %0d pops, %0d lines", n_events, n_pops, n_lines);
        if (n_lines == 0)
        begin
            $display("TESTBENCH RESULT: FAIL (no vectors; run make -C ../audio vectors)");
            $finish(1);
        end

        // ---- reset: both /R outputs high while nRESET is low -------------
        nRESET = 0;
        repeat (20) @(posedge CLK);
        #1;
        if (nSAMPLE_R !== 4'b1111) fail("nSAMPLE_R not high through reset");
        if (nCONTROL_R !== 1'b1) fail("nCONTROL_R not high through reset");
        // The edge just passed is m = 0: the last edge with reset asserted.
        m = 0;
        line = 0;
        next_event = 0;
        next_pop = 0;
        nRESET = 1;
        // Line 0's deposits, right after edge 0.
        apply_line_events(0);

        // ---- the run -----------------------------------------------------
        while (line < n_lines)
        begin
            @(posedge CLK);
            m = m + 1;
            #1;

            // 1. Pops: at a step edge the pin must match the model; at every
            //    other edge it must be low (one-clock pulse).
            if (m % 16 == 0)
            begin
                step = m / 16 - 1;
                expected_pop = 4'b0000;
                while (next_pop < n_pops && pops[next_pop][39:8] == (`MUTATE == 1 ? step - 1 : step))
                begin
                    expected_pop[pops[next_pop][1:0]] = 1'b1;
                    next_pop = next_pop + 1;
                end
                if (next_pop < n_pops && pops[next_pop][39:8] < (`MUTATE == 1 ? step - 1 : step))
                begin
                    fail("model pop without a matching step (vector order)");
                    next_pop = next_pop + 1;
                end
                if (nSAMPLE_R !== expected_pop)
                begin
                    fail("nSAMPLE_R differs from the model's pops at this step");
                end
                pop_checks = pop_checks + 1;
            end
            else
            begin
                // MUTATE 5 stretches the observed pulse to two clocks, as a chip
                // holding /R high one clock too long would look.
                obs_nsample_r = (`MUTATE == 5) ? (nSAMPLE_R | prev_nsample_r) : nSAMPLE_R;
                if (obs_nsample_r !== 4'b0000)
                begin
                    fail("nSAMPLE_R high off a step edge (pulse longer than one clock?)");
                end
                pulse_checks = pulse_checks + 1;
            end

            // 2. Gate law on every clock after the first.
            if (m >= 2)
            begin
                frame_tick = ((m - 1) % 512) >> 3;
                for (c = 0; c < 4; c = c + 1)
                begin
                    on_bit = on_window(volume_pre[c], frame_tick[5:0]);
                    expected_gate[c] = on_bit & ((`MUTATE == 3) ? 1'b1 : ~empty_pre[c]) & ~nSAMPLE_R[c];
                end
                if (GATE !== expected_gate)
                begin
                    fail("GATE differs from the closed-form law");
                end
                gate_checks = gate_checks + 1;
            end

            // 3. Control reader: /R low runs are exactly two clocks.
            if (nCONTROL_R == 1'b0)
            begin
                control_low_run = control_low_run + 1;
                if (control_low_run > 2) fail("nCONTROL_R low for more than two clocks");
            end
            else
            begin
                if (control_low_run == 1) fail("nCONTROL_R low for only one clock");
                control_low_run = 0;
            end

            // Line end: registers against the model; then the next line's deposits.
            if (m % 800 == 0)
            begin
                check_regs(line);
                line = line + 1;
                if (line < n_lines) apply_line_events(line);
            end
        end

        // FIFO protocol errors from the behavioural parts.
        if (control.reads_while_empty != 0) fail("control FIFO read while empty");
        $display("checks: %0d pop edges, %0d off-edge pulse checks, %0d gate clocks, %0d register lines; %0d pops consumed of %0d",
                 pop_checks, pulse_checks, gate_checks, reg_checks, next_pop, n_pops);
        if (next_pop != n_pops) fail("model pops left unconsumed");
        if (errors == 0)
        begin
            $display("TESTBENCH RESULT: PASS");
            $finish(0);
        end
        else
        begin
            $display("TESTBENCH RESULT: FAIL (%0d errors)", errors);
            $finish(1);
        end
    end

    task apply_line_events;
        input integer k;
        begin
            while (next_event < n_events && stim[next_event][39:16] == k)
            begin
                if (stim[next_event][15:12] == 4'd1)
                begin
                    control.write(stim[next_event][7:0]);
                end
                else
                begin
                    case (stim[next_event][11:8])
                        4'd0: sample0.write(stim[next_event][7:0]);
                        4'd1: sample1.write(stim[next_event][7:0]);
                        4'd2: sample2.write(stim[next_event][7:0]);
                        default: sample3.write(stim[next_event][7:0]);
                    endcase
                end
                next_event = next_event + 1;
            end
            if (next_event < n_events && stim[next_event][39:16] < k)
            begin
                fail("stimulus out of order");
                next_event = next_event + 1;
            end
        end
    endtask

    task check_regs;
        input integer k;
        reg [63:0] expected;
        begin
            expected = regs[(`MUTATE == 4) ? ((k == 0) ? 0 : k - 1) : k];
            if ({dut.period0, dut.period1, dut.period2, dut.period3,
                 2'b00, dut.volume0, 2'b00, dut.volume1, 2'b00, dut.volume2, 2'b00, dut.volume3} !== expected)
            begin
                fail("PERIOD/VOLUME registers differ from the model at the line's end");
            end
            reg_checks = reg_checks + 1;
        end
    endtask

endmodule
