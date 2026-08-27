// uart_echo_top - Arty A7-100T UART bring-up design.
//
// Two independent checks, so you can tell an RX problem from a TX problem:
//
//   echo    every byte received is sent straight back  (RX + TX together)
//   btn[0]  transmits a fixed banner                   (TX alone)
//
// led[3:0] shows the low nibble of the last byte received, so you can also
// confirm RX with no terminal at all: press 'A' (0x41) and led[0] lights.
//
// Port names match the Digilent master XDC. Directions are from the PC's
// point of view, which is the usual place to get this wrong:
//   uart_rxd_out  D10  the PC's RX line  -> FPGA OUTPUT
//   uart_txd_in   A9   the PC's TX line  -> FPGA INPUT

`default_nettype none

module uart_echo_top #(
    parameter integer CLK_HZ = 100_000_000,
    parameter integer BAUD   = 115_200
)(
    input  wire       CLK100MHZ,
    input  wire       uart_txd_in,     // from PC
    output wire       uart_rxd_out,    // to PC
    input  wire [3:0] btn,
    output reg  [3:0] led
);
    wire       rst = 1'b0;             // no board reset wired; POR is enough

    wire [7:0] rx_data;
    wire       rx_valid, rx_frame_err;

    reg  [7:0] tx_data;
    reg        tx_start;
    wire       tx_ready;

    uart_rx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_rx (
        .clk(CLK100MHZ), .rst(rst), .rx(uart_txd_in),
        .data(rx_data), .valid(rx_valid), .frame_err(rx_frame_err)
    );

    uart_tx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) u_tx (
        .clk(CLK100MHZ), .rst(rst), .data(tx_data), .start(tx_start),
        .tx(uart_rxd_out), .ready(tx_ready)
    );

    // ---- banner ROM: "ARTY A7-100T UART OK\r\n" ----------------------
    localparam integer BANNER_LEN = 22;

    function [7:0] banner(input [4:0] i);
        case (i)
            5'd0:  banner = "A";  5'd1:  banner = "R";  5'd2:  banner = "T";
            5'd3:  banner = "Y";  5'd4:  banner = " ";  5'd5:  banner = "A";
            5'd6:  banner = "7";  5'd7:  banner = "-";  5'd8:  banner = "1";
            5'd9:  banner = "0";  5'd10: banner = "0";  5'd11: banner = "T";
            5'd12: banner = " ";  5'd13: banner = "U";  5'd14: banner = "A";
            5'd15: banner = "R";  5'd16: banner = "T";  5'd17: banner = " ";
            5'd18: banner = "O";  5'd19: banner = "K";  5'd20: banner = 8'h0D;
            5'd21: banner = 8'h0A;
            default: banner = 8'h00;
        endcase
    endfunction

    // ---- button: synchronise, debounce, take the rising edge ----------
    localparam integer DB_TICKS = CLK_HZ / 1000;    // 1 ms
    localparam integer DBW = (DB_TICKS <= 2) ? 1 : $clog2(DB_TICKS);

    reg           b_meta, b_sync, b_stable, b_prev;
    reg [DBW-1:0] db_cnt;
    wire          btn_rise = b_stable & ~b_prev;

    always @(posedge CLK100MHZ) begin
        b_meta <= btn[0];
        b_sync <= b_meta;
        b_prev <= b_stable;
        if (b_sync != b_stable) begin
            if (db_cnt == (DB_TICKS-1)) begin
                b_stable <= b_sync;
                db_cnt   <= 0;
            end else begin
                db_cnt <= db_cnt + 1'b1;
            end
        end else begin
            db_cnt <= 0;
        end
    end

    // ---- echo / banner arbiter ---------------------------------------
    // rx_valid can arrive while tx is busy, so the byte is parked in a
    // one-deep holding register instead of being dropped on the floor.
    reg [4:0] b_idx;
    reg       b_active;
    reg [7:0] pend_data;
    reg       pend_valid;

    initial begin
        led = 4'd0; tx_data = 8'd0; tx_start = 1'b0;
        b_idx = 5'd0; b_active = 1'b0; pend_data = 8'd0; pend_valid = 1'b0;
        b_meta = 1'b0; b_sync = 1'b0; b_stable = 1'b0; b_prev = 1'b0; db_cnt = 0;
    end

    always @(posedge CLK100MHZ) begin
        tx_start <= 1'b0;

        if (rx_valid) begin
            led        <= rx_data[3:0];
            pend_data  <= rx_data;
            pend_valid <= 1'b1;
        end

        if (btn_rise && !b_active) begin
            b_active <= 1'b1;
            b_idx    <= 5'd0;
        end

        if (tx_ready && !tx_start) begin
            if (b_active) begin
                tx_data  <= banner(b_idx);
                tx_start <= 1'b1;
                if (b_idx == (BANNER_LEN-1)) b_active <= 1'b0;
                else                         b_idx    <= b_idx + 1'b1;
            end else if (pend_valid) begin
                tx_data    <= pend_data;
                tx_start   <= 1'b1;
                pend_valid <= 1'b0;
            end
        end
    end
endmodule

`default_nettype wire
