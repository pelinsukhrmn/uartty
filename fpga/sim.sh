#!/bin/sh
# Runs both testbenches with Icarus Verilog. No Vivado needed.
set -e
iverilog -g2012 -o tb.out  tb_uart.v  uart_echo_top.v uart_rx.v uart_tx.v
iverilog -g2012 -o tbr.out tb_rates.v uart_echo_top.v uart_rx.v uart_tx.v
echo "== functional =="; vvp tb.out
echo; echo "== real board rates =="; vvp tbr.out
