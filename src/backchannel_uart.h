#pragma once

#include "typedefs.h"
#include "ring_buffer.h"

#define COMMAND_SIZE 2
#define COMMAND_COUNT 29

extern Ring_Buffer bc_tx;
extern Ring_Buffer bc_rx;
extern int TX_MODE;

extern void (*CHECK_FOR_COMMAND_FUNC)(void);
extern char COMMANDS[COMMAND_COUNT][COMMAND_SIZE];
extern void (*COMMAND_FUNC[COMMAND_COUNT])(void);

void bc_init();

void bc_print(const char * str);

void bc_print_crlf(const char * str);

void bc_print_byte(i8 byte, i8 base);

void bc_print_raw(i8 byte);



static void _radio_clear_interrupts();


static void _radio_set_config_power_up_rx();

static void _radio_set_config_power_up_tx();

static void _radio_get_config();


static void _radio_set_freq_channel();

static void _radio_get_freq_channel();


static void _radio_set_rf_setup_normal();

static void _radio_set_rf_setup_carrier();

static void _radio_get_rf_setup();


static void _radio_set_retransmission();

static void _radio_get_retransmission();


static void _radio_set_address_width();

static void _radio_get_address_width();


static void _radio_set_rx_pipe_enable();

static void _radio_get_rx_pipe_enable();


static void _radio_set_rx_pipe_0_address();

static void _radio_get_rx_pipe_0_address();


static void _radio_set_tx_address();

static void _radio_get_tx_address();


static void _radio_set_auto_ack();

static void _radio_get_auto_ack();


static void _radio_set_pipe_dynamic_payload();

static void _radio_get_pipe_dynamic_payload();


static void _radio_set_features();

static void _radio_get_features();


static void _radio_get_fifo_status();

static void _radio_get_packet_stats();

static void _radio_get_rx_power();

static void _radio_toggle_enable();


static void _check_command();

static void _uart_init();

static void _pin_init();


static inline void _send_next();