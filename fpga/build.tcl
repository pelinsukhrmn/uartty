# Non-project batch build. From this directory:
#   vivado -mode batch -source build.tcl
# Produces uart_echo_top.bit
set part xc7a100tcsg324-1
set top  uart_echo_top

read_verilog [list uart_rx.v uart_tx.v uart_echo_top.v]
read_xdc     arty_a7_100t.xdc

synth_design -top $top -part $part
opt_design
place_design
route_design

report_timing_summary -file timing.rpt
report_utilization    -file util.rpt

write_bitstream -force ${top}.bit
puts "wrote ${top}.bit"
