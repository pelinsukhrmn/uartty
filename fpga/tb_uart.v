// Self-checking testbench for uart_echo_top.
//
// A background monitor decodes the TX pin into a queue continuously, so the
// stimulus can keep driving RX without missing the echo that overlaps it.
// Scaled-down clock/baud so it runs fast; the RTL is rate-independent.

`timescale 1ns/1ps

module tb_uart;
    localparam integer CLK_HZ = 1_000_000;
    localparam integer BAUD   =    62_500;    // DIV = 16
    localparam integer BIT_NS = 1_000_000_000 / BAUD;
    localparam integer CLK_NS = 1_000_000_000 / CLK_HZ;
    // the DUT debounces btn[0] over 1 ms, so the press must outlast that
    localparam integer DB_NS  = 1_000_000;

    reg        clk = 0;
    reg        rx_pin = 1'b1;                 // idle high
    wire       tx_pin;
    reg  [3:0] btn = 4'd0;
    wire [3:0] led;

    integer errors = 0;
    integer i;

    always #(CLK_NS/2) clk = ~clk;

    uart_echo_top #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) dut (
        .CLK100MHZ(clk), .uart_txd_in(rx_pin), .uart_rxd_out(tx_pin),
        .btn(btn), .led(led)
    );

    // ---- background TX monitor -------------------------------------
    reg [7:0] q [0:511];
    integer   qw = 0, qr = 0;
    integer   tx_frame_errors = 0;

    initial begin : monitor
        reg [7:0] b;
        integer   k;
        forever begin
            @(negedge tx_pin);                     // start bit
            #(BIT_NS + BIT_NS/2);                  // middle of bit 0
            for (k = 0; k < 8; k = k + 1) begin
                b[k] = tx_pin;
                #(BIT_NS);
            end
            if (tx_pin === 1'b1) begin             // middle of stop bit
                q[qw] = b;
                qw = qw + 1;
            end else begin
                tx_frame_errors = tx_frame_errors + 1;
            end
        end
    end

    task send_byte(input [7:0] b);
        integer k;
        begin
            rx_pin = 1'b0; #(BIT_NS);              // start
            for (k = 0; k < 8; k = k + 1) begin
                rx_pin = b[k]; #(BIT_NS);          // LSB first
            end
            rx_pin = 1'b1; #(BIT_NS);              // stop
        end
    endtask

    task get_byte(output [7:0] b, output ok);
        integer guard;
        begin
            guard = 0;
            while (qr >= qw && guard < 400) begin
                #(BIT_NS); guard = guard + 1;
            end
            if (qr < qw) begin b = q[qr]; qr = qr + 1; ok = 1'b1; end
            else         begin b = 8'hxx;             ok = 1'b0; end
        end
    endtask

    task check(input string label, input cond);
        begin
            if (cond) $display("PASS  %0s", label);
            else begin $display("FAIL  %0s", label); errors = errors + 1; end
        end
    endtask

    reg [7:0] got;
    reg       ok;
    reg [7:0] seq [0:3];
    string    banner_s = "ARTY A7-100T UART OK";

    initial begin
        seq[0] = "A"; seq[1] = 8'h00; seq[2] = 8'hFF; seq[3] = "z";

        #(10*CLK_NS);

        // 1. plain echo
        send_byte("A");
        get_byte(got, ok);
        check("echo 'A'", ok && got == "A");
        check("led shows low nibble of 'A'", led == 4'h1);      // 'A' = 0x41

        // 2. edge-case byte values
        send_byte(8'h00); get_byte(got, ok); check("echo 0x00", ok && got == 8'h00);
        send_byte(8'hFF); get_byte(got, ok); check("echo 0xFF", ok && got == 8'hFF);
        send_byte("z");   get_byte(got, ok); check("echo 'z'",  ok && got == "z");

        // 3. four bytes back to back with no idle gap - none may be dropped
        //    while the transmitter is still busy with the previous one
        send_byte("1"); send_byte("2"); send_byte("3"); send_byte("4");
        get_byte(got, ok); check("stream byte 1", ok && got == "1");
        get_byte(got, ok); check("stream byte 2", ok && got == "2");
        get_byte(got, ok); check("stream byte 3", ok && got == "3");
        get_byte(got, ok); check("stream byte 4", ok && got == "4");

        // 4. framing error: hold the stop bit low, expect no echo
        rx_pin = 1'b0; #(BIT_NS);
        for (i = 0; i < 8; i = i + 1) begin rx_pin = 1'b1; #(BIT_NS); end
        rx_pin = 1'b0; #(BIT_NS);                  // bad stop bit
        rx_pin = 1'b1; #(4*BIT_NS);
        check("framing error suppresses echo", qr >= qw);

        // 5. a low glitch far shorter than a bit must not look like a start
        rx_pin = 1'b0; #(BIT_NS/4); rx_pin = 1'b1; #(4*BIT_NS);
        check("glitch rejected, no phantom byte", qr >= qw);

        // 6. banner on button press
        btn[0] = 1'b1; #(3*DB_NS); btn[0] = 1'b0;
        for (i = 0; i < 20; i = i + 1) begin
            get_byte(got, ok);
            check($sformatf("banner[%0d] = '%0s'", i, banner_s[i]),
                  ok && got == banner_s[i]);
        end
        get_byte(got, ok); check("banner CR", ok && got == 8'h0D);
        get_byte(got, ok); check("banner LF", ok && got == 8'h0A);

        check("no framing errors on the TX pin", tx_frame_errors == 0);

        $display("");
        if (errors == 0) $display("ALL TESTS PASSED");
        else             $display("%0d FAILURE(S)", errors);
        $finish;
    end

    initial begin
        #(200_000_000);
        $display("TIMEOUT");
        $finish;
    end
endmodule
