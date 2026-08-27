// uart_tx - 8N1 transmitter. Assert start for one cycle while ready is high.

`default_nettype none

module uart_tx #(
    parameter integer CLK_HZ = 100_000_000,
    parameter integer BAUD   = 115_200
)(
    input  wire       clk,
    input  wire       rst,
    input  wire [7:0] data,
    input  wire       start,      // one-cycle pulse, ignored unless ready
    output reg        tx,
    output wire       ready       // high when idle
);
    localparam integer DIV = CLK_HZ / BAUD;
    localparam integer CW  = (DIV <= 2) ? 1 : $clog2(DIV);

    reg [CW-1:0] cnt;
    reg [3:0]    bitn;
    reg [9:0]    sh;               // {stop, data[7:0], start}
    reg          busy;

    assign ready = ~busy;

    initial begin
        tx = 1'b1; busy = 1'b0; cnt = 0; bitn = 0; sh = 10'h3FF;
    end

    always @(posedge clk) begin
        if (rst) begin
            tx   <= 1'b1;
            busy <= 1'b0;
            cnt  <= 0;
            bitn <= 0;
            sh   <= 10'h3FF;
        end else if (!busy) begin
            tx <= 1'b1;
            if (start) begin
                sh   <= {1'b1, data, 1'b0};
                tx   <= 1'b0;      // start bit goes out immediately
                busy <= 1'b1;
                cnt  <= 0;
                bitn <= 0;
            end
        end else begin
            if (cnt == (DIV-1)) begin
                cnt <= 0;
                if (bitn == 4'd9) begin
                    busy <= 1'b0;  // stop bit has been held a full bit time
                    tx   <= 1'b1;
                end else begin
                    bitn <= bitn + 1'b1;
                    tx   <= sh[bitn + 1];
                end
            end else begin
                cnt <= cnt + 1'b1;
            end
        end
    end
endmodule

`default_nettype wire
