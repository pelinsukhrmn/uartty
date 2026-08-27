## Arty A7-100T (XC7A100T-1CSG324C) - constraints for uart_echo_top.
## Pin locations taken from Digilent's Arty-A7-100-Master.xdc.
##
## Direction note, since this is the usual place to get it wrong: the signal
## names are from the PC's point of view, not the FPGA's.
##   uart_rxd_out  the PC's receive line  -> FPGA OUTPUT (our TX)
##   uart_txd_in   the PC's transmit line -> FPGA INPUT  (our RX)

## Clock - 100 MHz
set_property -dict { PACKAGE_PIN E3  IOSTANDARD LVCMOS33 } [get_ports { CLK100MHZ }];
create_clock -add -name sys_clk_pin -period 10.00 -waveform {0 5} [get_ports { CLK100MHZ }];

## USB-UART (FT2232HQ channel B)
set_property -dict { PACKAGE_PIN D10 IOSTANDARD LVCMOS33 } [get_ports { uart_rxd_out }];
set_property -dict { PACKAGE_PIN A9  IOSTANDARD LVCMOS33 } [get_ports { uart_txd_in }];

## LEDs - low nibble of the last byte received
set_property -dict { PACKAGE_PIN H5  IOSTANDARD LVCMOS33 } [get_ports { led[0] }];
set_property -dict { PACKAGE_PIN J5  IOSTANDARD LVCMOS33 } [get_ports { led[1] }];
set_property -dict { PACKAGE_PIN T9  IOSTANDARD LVCMOS33 } [get_ports { led[2] }];
set_property -dict { PACKAGE_PIN T10 IOSTANDARD LVCMOS33 } [get_ports { led[3] }];

## Buttons - btn[0] transmits the banner
set_property -dict { PACKAGE_PIN D9  IOSTANDARD LVCMOS33 } [get_ports { btn[0] }];
set_property -dict { PACKAGE_PIN C9  IOSTANDARD LVCMOS33 } [get_ports { btn[1] }];
set_property -dict { PACKAGE_PIN B9  IOSTANDARD LVCMOS33 } [get_ports { btn[2] }];
set_property -dict { PACKAGE_PIN B8  IOSTANDARD LVCMOS33 } [get_ports { btn[3] }];

## The UART pins are asynchronous to sys_clk. uart_rx.v resynchronises the
## input with a two-flop chain, so tell the timer not to try to close a path
## that has no meaningful launch clock.
set_false_path -from [get_ports uart_txd_in]
set_false_path -to   [get_ports uart_rxd_out]
set_false_path -from [get_ports { btn[*] }]
set_false_path -to   [get_ports { led[*] }]

## Configuration - lets the bitstream boot from the QSPI flash if you write it
set_property CONFIG_VOLTAGE 3.3        [current_design]
set_property CFGBVS VCCO                [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4      [current_design]
set_property BITSTREAM.CONFIG.CONFIGRATE 33       [current_design]
set_property CONFIG_MODE SPIx4                    [current_design]
