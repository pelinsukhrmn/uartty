// Runs the real board parameters (100 MHz clock) at 115200 and 1 Mbaud, and
// checks the DIV arithmetic the design relies on.
`timescale 1ns/1ps

module tb_rates;
    localparam integer CLK_HZ = 100_000_000;
    integer errors = 0;

    // --- 115200 ---
    localparam integer BAUD_A = 115_200;
    localparam integer DIV_A  = CLK_HZ / BAUD_A;
    // --- 1 Mbaud ---
    localparam integer BAUD_B = 1_000_000;
    localparam integer DIV_B  = CLK_HZ / BAUD_B;

    reg clk = 0;
    always #5 clk = ~clk;                   // 100 MHz

    reg  rx_a = 1'b1; wire tx_a; wire [3:0] led_a;
    reg  rx_b = 1'b1; wire tx_b; wire [3:0] led_b;

    uart_echo_top #(.CLK_HZ(CLK_HZ), .BAUD(BAUD_A)) dut_a (
        .CLK100MHZ(clk), .uart_txd_in(rx_a), .uart_rxd_out(tx_a),
        .btn(4'd0), .led(led_a));
    uart_echo_top #(.CLK_HZ(CLK_HZ), .BAUD(BAUD_B)) dut_b (
        .CLK100MHZ(clk), .uart_txd_in(rx_b), .uart_rxd_out(tx_b),
        .btn(4'd0), .led(led_b));

    localparam integer BNS_A = 1_000_000_000 / BAUD_A;
    localparam integer BNS_B = 1_000_000_000 / BAUD_B;

    task drive_a(input [7:0] b);
        integer k;
        begin
            rx_a = 1'b0; #(BNS_A);
            for (k = 0; k < 8; k = k + 1) begin rx_a = b[k]; #(BNS_A); end
            rx_a = 1'b1; #(BNS_A);
        end
    endtask

    task drive_b(input [7:0] b);
        integer k;
        begin
            rx_b = 1'b0; #(BNS_B);
            for (k = 0; k < 8; k = k + 1) begin rx_b = b[k]; #(BNS_B); end
            rx_b = 1'b1; #(BNS_B);
        end
    endtask

    task grab_a(output [7:0] b, output ok);
        integer k; integer guard;
        begin
            b = 0; ok = 0; guard = 0;
            while (tx_a !== 1'b0 && guard < 60) begin #(BNS_A); guard = guard + 1; end
            if (tx_a !== 1'b0) disable grab_a;
            #(BNS_A + BNS_A/2);
            for (k = 0; k < 8; k = k + 1) begin b[k] = tx_a; #(BNS_A); end
            ok = (tx_a === 1'b1);
        end
    endtask

    task grab_b(output [7:0] b, output ok);
        integer k; integer guard;
        begin
            b = 0; ok = 0; guard = 0;
            while (tx_b !== 1'b0 && guard < 60) begin #(BNS_B); guard = guard + 1; end
            if (tx_b !== 1'b0) disable grab_b;
            #(BNS_B + BNS_B/2);
            for (k = 0; k < 8; k = k + 1) begin b[k] = tx_b; #(BNS_B); end
            ok = (tx_b === 1'b1);
        end
    endtask

    task check(input string label, input cond);
        begin
            if (cond) $display("PASS  %0s", label);
            else begin $display("FAIL  %0s", label); errors = errors + 1; end
        end
    endtask

    reg [7:0] got; reg ok;
    real err_a, err_b;

    initial begin
        $display("115200: DIV=%0d  actual=%0d baud", DIV_A, CLK_HZ/DIV_A);
        $display("1 Mbd : DIV=%0d  actual=%0d baud", DIV_B, CLK_HZ/DIV_B);
        err_a = 100.0 * ((CLK_HZ*1.0/DIV_A) - BAUD_A) / BAUD_A;
        err_b = 100.0 * ((CLK_HZ*1.0/DIV_B) - BAUD_B) / BAUD_B;
        $display("baud error: 115200 -> %0.4f%%   1 Mbaud -> %0.4f%%", err_a, err_b);
        $display("");

        check("DIV(115200) = 868", DIV_A == 868);
        check("DIV(1 Mbaud) = 100 exactly", DIV_B == 100 && CLK_HZ % BAUD_B == 0);
        check("115200 baud error under 2%", (err_a < 2.0) && (err_a > -2.0));

        #1000;

        // 1 Mbaud instance first (it is 8.7x faster, so it finishes sooner)
        drive_b("Q");
        grab_b(got, ok);
        check("echo at 1 Mbaud on a 100 MHz clock", ok && got == "Q");

        drive_a("Q");
        grab_a(got, ok);
        check("echo at 115200 on a 100 MHz clock", ok && got == "Q");

        $display("");
        if (errors == 0) $display("ALL TESTS PASSED");
        else             $display("%0d FAILURE(S)", errors);
        $finish;
    end

    initial begin #50_000_000; $display("TIMEOUT"); $finish; end
endmodule
