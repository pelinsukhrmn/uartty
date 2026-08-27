// uart_rx - 8N1 receiver, mid-bit sampling.
//
// DIV = CLK_HZ / BAUD. The input is resynchronised before use: the pin is
// asynchronous to clk and sampling it directly is a metastability bug that
// shows up as rare, unreproducible corrupt bytes.

`default_nettype none

module uart_rx #(
    parameter integer CLK_HZ = 100_000_000,
    parameter integer BAUD   = 115_200
)(
    input  wire       clk,
    input  wire       rst,        // synchronous, active high
    input  wire       rx,         // raw pin
    output reg  [7:0] data,
    output reg        valid,      // one-cycle pulse, data is good
    output reg        frame_err   // one-cycle pulse, stop bit was not 1
);
    localparam integer DIV  = CLK_HZ / BAUD;
    localparam integer HALF = DIV / 2;
    localparam integer CW   = (DIV <= 2) ? 1 : $clog2(DIV);

    localparam [1:0] S_IDLE = 2'd0, S_START = 2'd1, S_DATA = 2'd2, S_STOP = 2'd3;

    reg rx_meta, rx_sync;
    always @(posedge clk) begin
        rx_meta <= rx;
        rx_sync <= rx_meta;
    end

    reg [1:0]    state;
    reg [CW-1:0] cnt;
    reg [2:0]    bitn;
    reg [7:0]    sh;

    initial begin
        state = S_IDLE; cnt = 0; bitn = 0; sh = 0;
        data = 0; valid = 0; frame_err = 0;
        rx_meta = 1'b1; rx_sync = 1'b1;
    end

    always @(posedge clk) begin
        valid     <= 1'b0;
        frame_err <= 1'b0;

        if (rst) begin
            state <= S_IDLE;
            cnt   <= 0;
            bitn  <= 0;
            sh    <= 8'd0;
            data  <= 8'd0;
        end else begin
            case (state)
            S_IDLE: begin
                cnt  <= 0;
                bitn <= 0;
                if (!rx_sync) state <= S_START;
            end

            S_START: begin
                // Re-check at the middle of the start bit. A line glitch
                // that has gone away by now is not a start bit.
                if (cnt == HALF[CW-1:0]) begin
                    cnt <= 0;
                    state <= rx_sync ? S_IDLE : S_DATA;
                end else begin
                    cnt <= cnt + 1'b1;
                end
            end

            S_DATA: begin
                if (cnt == (DIV-1)) begin
                    cnt <= 0;
                    sh  <= {rx_sync, sh[7:1]};      // LSB first
                    if (bitn == 3'd7) state <= S_STOP;
                    else              bitn  <= bitn + 1'b1;
                end else begin
                    cnt <= cnt + 1'b1;
                end
            end

            S_STOP: begin
                if (cnt == (DIV-1)) begin
                    cnt   <= 0;
                    state <= S_IDLE;
                    data  <= sh;
                    if (rx_sync) valid     <= 1'b1;
                    else         frame_err <= 1'b1;
                end else begin
                    cnt <= cnt + 1'b1;
                end
            end

            default: state <= S_IDLE;
            endcase
        end
    end
endmodule

`default_nettype wire
