#include <msp430.h>

#include "node_control.h"
#include "backchannel_uart.h"
#include "radio_nrf24l01p.h"
#include "rtc.h"
#include "soil_sensor.h"

volatile Node_Control nctrl = {};
volatile Timeslot_Packet pckt = {};
volatile u8 rx_synced = 0;
volatile u8 rx_frame_synced = 0;
volatile u16 prev_sample = 0;
u16 root_node_data[MAX_NODE_HOPS * MAX_TIMESLOTS_PER_FRAME] = {};

static void _sample_callback()
{
    OUR_TIMESLOT_DATA.data.data = soil_sensor_get_latest_sample();
    root_node_data[OUR_TIMESLOT_DATA.data.src_addr] = OUR_TIMESLOT_DATA.data.data;
    if ((OUR_TIMESLOT_DATA.data.data > (prev_sample + DELTA_TO_SEND)) || ((OUR_TIMESLOT_DATA.data.data + DELTA_TO_SEND) < prev_sample))
    {
        OUR_TIMESLOT_DATA.data.dest_addr = 0x01;

        // If there aren't any timeslots with our dest addr (ie root), mark the data so - using the upper most bit
        // since our ADC is only 10 bits - the top few bits of the data are free for use
        u16 mask = NODE_OUTSIDE_NEIGHBORHOOD;
        for (i8 i = 0; i < nctrl.timeslots_per_frame; ++i)
        {
            i8 ts = i + 1;
            if (TS_DATA(ts).data.src_addr == OUR_TIMESLOT_DATA.data.dest_addr)
                mask = 0;
        }
        //OUR_TIMESLOT_DATA.data.data |= mask;
        prev_sample = OUR_TIMESLOT_DATA.data.data;
    }
}

#ifndef RADIO_DEBUG_SPI
void _clear_timeslot(i8 ind)
{
    nctrl.cur_frame.timeslots[ind].data.data = 0x00;
    nctrl.cur_frame.timeslots[ind].data.src_addr = 0x00;
    nctrl.cur_frame.timeslots[ind].data.dest_addr = 0x00;
    nctrl.cur_frame.timeslots[ind].no_rx_count = 0x00;
    nctrl.cur_frame.timeslots[ind].timeslot_mask = 0x00;
}

void send_end_frame_state()
{
    bc_send((u8*)&nctrl.timeslots_per_frame,1);
    bc_send((u8*)&nctrl.startup_listen_frame_count,1);
    bc_send((u8*)&nctrl.sleep_frame_count,1);
    bc_send((u8*)&nctrl.total_node_count,1);
    bc_send((u8*)&nctrl.cur_frame.our_timeslot,1);
    bc_send((u8*)&nctrl.cur_frame.ind,1);
    bc_send((u8*)&nctrl.cur_frame.remove_next_frame,1);
    bc_send((u8*)&nctrl.cur_frame.remove_this_frame,1);
    bc_send((u8*)&nctrl.src_t,8);
    bc_send((u8*)&nctrl.t,18);
    bc_send((u8*)root_node_data, 16); // Should be 145 + 4 = 149
}

void _clock_in_payload()
{
    pckt.frame_ind = nctrl.cur_frame.ind;
    pckt.timeslot = nctrl.cur_frame.cur_timeslot;

    // The TX always counts as a taken timeslot
    _set_timeslot_bit();
    pckt.timeslot_mask = OUR_TIMESLOT_DATA.timeslot_mask;
    pckt.total_node_count = nctrl.total_node_count;
    pckt.future2 = 134;

    pckt.data.src_addr = OUR_TIMESLOT_DATA.data.src_addr;
    pckt.data.dest_addr = OUR_TIMESLOT_DATA.data.dest_addr;
    pckt.data.data = OUR_TIMESLOT_DATA.data.data;
    if (OUR_TIMESLOT_DATA.data.dest_addr)
        OUR_TIMESLOT_DATA.data.dest_addr = 0;

    // We want to copy these no matter what - as they are filled in when receiving packets as needed
    for (i8 i = 0; i < MAX_NODE_HOPS; ++i)
        pckt.fwd[i] = nctrl.cur_frame.our_fwds[i];

    if (nctrl.cur_frame.remove_this_frame)
        pckt.removed_node_addr = nctrl.cur_frame.remove_this_frame;

    radio_clock_in(pckt.raw_data, RADIO_PAYLOAD_SIZE);
    bc_send((u8*)pckt.raw_data, RADIO_PAYLOAD_SIZE);
}

void _clock_out_payload()
{
    radio_clock_out(pckt.raw_data, RADIO_PAYLOAD_SIZE);
    nctrl.cur_frame.ind = pckt.frame_ind;
    nctrl.cur_frame.cur_timeslot = pckt.timeslot;
    CUR_TIMESLOT_DATA.timeslot_mask = pckt.timeslot_mask;
    CUR_TIMESLOT_DATA.data = pckt.data;

    // Add this timeslot to our timeslot mask entry, if our timeslot has been selected (not our first frame)
    // Do forward logic stuff here

    if (nctrl.cur_frame.our_timeslot && (CUR_TIMESLOT_DATA.data.dest_addr == OUR_TIMESLOT_DATA.data.src_addr))
    {
        root_node_data[CUR_TIMESLOT_DATA.data.src_addr] = CUR_TIMESLOT_DATA.data.data;
    }

    if (pckt.total_node_count > nctrl.total_node_count)
        nctrl.total_node_count = pckt.total_node_count;

    if (pckt.removed_node_addr && pckt.removed_node_addr != nctrl.cur_frame.remove_this_frame)
        nctrl.cur_frame.remove_next_frame = pckt.removed_node_addr;

    // Set the no receive count to zero
    CUR_TIMESLOT_DATA.no_rx_count = 0;
    bc_send((u8*)pckt.raw_data,RADIO_PAYLOAD_SIZE);
}

void tx_packet_sent()
{
    P1OUT &= ~BIT4;
    radio_configure(RADIO_RX);
}

void _sync_new_timeslot()
{
    i16 occupied_mask = 0;
    i16 my_occupied_mask = 0;
    i8 first_occupied = 0;
    for (i8 i = 0; i < nctrl.timeslots_per_frame; ++i)
    {
        // Set the first occupied slot so we know where to search after... we can't try to take a node before this because
        // we may have started up in the middle of the frame (however unlikely)
        if (!first_occupied && nctrl.cur_frame.timeslots[i].timeslot_mask)
            first_occupied = i + 1;

        // Or the occupied timeslot masks of the other nodes in our neighborhood - the gives me the occupied timeslots within 2 hops
        occupied_mask |= nctrl.cur_frame.timeslots[i].timeslot_mask;

        // Set the bit in my occupied mask - this is basically the nodes in my immediate neighborhood
        if (nctrl.cur_frame.timeslots[i].timeslot_mask)
            my_occupied_mask |= (0x0001 << i);
    }

    // Find first open, but must be after our first occupied (to make sure we have heard if there is anything to hear)
    i8 first_open = 0;
    for (i8 i = first_occupied; i < nctrl.timeslots_per_frame; ++i)
    {
        if (!((occupied_mask >> i) & 0x0001))
        {
            first_open = i + 1;
            break;
        }
    }
    nctrl.cur_frame.our_timeslot = first_open;

    // Increment the node count and set our address
    ++nctrl.total_node_count;
    OUR_TIMESLOT_DATA.data.src_addr = nctrl.total_node_count;

    // Always send data to source node
    OUR_TIMESLOT_DATA.data.dest_addr = 0x01;
}

void _handle_removed_nodes()
{
    if (nctrl.cur_frame.remove_this_frame)
    {
        --nctrl.total_node_count;
        for (i8 i = 0; i < nctrl.timeslots_per_frame; ++i)
        {
            i8 ts = i + 1;
            if (TS_DATA(ts).data.src_addr == nctrl.cur_frame.remove_this_frame)
                _clear_timeslot(i);

            if (TS_DATA(ts).data.src_addr > nctrl.cur_frame.remove_this_frame)
                --TS_DATA(ts).data.src_addr;
        }
        pckt.removed_node_addr = 0x00;
        nctrl.cur_frame.remove_this_frame = 0x00;
    }

    if (nctrl.cur_frame.remove_next_frame)
    {
        nctrl.cur_frame.remove_this_frame = nctrl.cur_frame.remove_next_frame;
        nctrl.cur_frame.remove_next_frame = 0x00;
    }
}

void frame_start()
{
    ++nctrl.cur_frame.cur_timeslot;
    if (nctrl.cur_frame.cur_timeslot == nctrl.cur_frame.our_timeslot)
    {
        // The frame after this HAS to be TX - only one rx frame and def not end of frame.. so...
        radio_configure(RADIO_TX);
        _clock_in_payload();
        rtc_set_cb(frame_tx_timeslot);
        rtc_set_tick_cycles(nctrl.t.tx_to_rx);
    }
    else
    {
        rtc_set_cb(frame_rx_timeslot_begin);
        rtc_set_tick_cycles(nctrl.t.rx_on);
    }
}

void frame_prep_start()
{
    rx_frame_synced = 0;

    // Need a long listen at the start of frame - until we get at least one RX packet
    _set_long_listen();
    rtc_set_interrupt_tick_count(0);
    rtc_set_cb(frame_start);

    // Trigger a fresh sensor sample
    soil_sensor_trigger_sample();
    toggel_pin_next_isr = 1;

    // Always setting the rtc clock ahead of the interrupt triggering - due to the shadow register
    if (nctrl.cur_frame.cur_timeslot + 1 == nctrl.cur_frame.our_timeslot)
        rtc_set_tick_cycles(nctrl.t.rx_packet_to_tx);
    else
        rtc_set_tick_cycles(nctrl.t.rx_packet_to_rx);
}

void frame_end()
{
    rtc_set_interrupt_tick_count(nctrl.sleep_frame_count * nctrl.timeslots_per_frame - 2);
    rtc_set_cb(frame_prep_start);
    ++nctrl.cur_frame.ind;
    nctrl.cur_frame.cur_timeslot = 0;

    _handle_removed_nodes();

    // If our timeslot is 0, that means this was our first sync timeslot and now we need to figure
    // out our address and timeslot so it will be sent out next frame
    if (!nctrl.cur_frame.our_timeslot)
        _sync_new_timeslot();

    // Ensure we get new sample data every frame
    OUR_TIMESLOT_DATA.data.data = 0x0000;
    OUR_TIMESLOT_DATA.data.dest_addr = 0x00;

    send_end_frame_state();
#ifdef BC_UART_DEBUG
    bc_print_int(nctrl.cur_frame.ind, 10);
    bc_print(" Frames\n\r\r\n");
    bc_print("\n\rNodes:");
    bc_print_byte(nctrl.total_node_count, 10);
    bc_print("\n\r");
    for (i8 i = 0; i < nctrl.timeslots_per_frame; ++i)
    {
        i8 ts = i + 1;
        bc_print("TS: ");
        bc_print_byte(ts, 10);
        bc_print(" Addr:");
        bc_print_int(TS_DATA(ts).data.src_addr, 10);
        bc_print(" Mask:");
        bc_print_int(TS_DATA(ts).timeslot_mask, 10);
        bc_print(" Data:");
        bc_print_int(TS_DATA(ts).data.data, 10);
        bc_print(" NORXCNT:");
        bc_print_int(TS_DATA(ts).no_rx_count, 10);
        bc_print("\n\r");
    }
#endif
}

void _rx_end_next_tx()
{
    radio_configure(RADIO_TX);
    _clock_in_payload();
    rtc_set_cb(frame_tx_timeslot);

    if ((nctrl.cur_frame.cur_timeslot + 1) > nctrl.timeslots_per_frame)
        rtc_set_tick_cycles(nctrl.t.tx_to_end_frame);
    else
        rtc_set_tick_cycles(nctrl.t.tx_to_rx);
}

void _rx_end_next_rx()
{
    rtc_set_cb(frame_rx_timeslot_begin);
    rtc_set_tick_cycles(nctrl.t.rx_on);
}

void _rx_end_next_frame_end()
{
    toggel_pin_next_isr = 1;
    rtc_set_cb(frame_end);
    rtc_set_tick_cycles(nctrl.t.timeslot);
}

void _reset_clock_to_cycles(i16 cycles)
{
    rtc_set_tick_cycles(cycles);
    rtc_start();
    rtc_reset();
    while (rtc_get_elapsed() != 1)
        ;
}

static void _set_timeslot_bit()
{
    OUR_TIMESLOT_DATA.timeslot_mask |= (0x0001 << (nctrl.cur_frame.cur_timeslot - 1));
}

static void _unset_timeslot_bit()
{
    OUR_TIMESLOT_DATA.timeslot_mask &= ~(0x0001 << (nctrl.cur_frame.cur_timeslot - 1));
}

void _prepare_next_timeslot(i16 cycles)
{
    _reset_clock_to_cycles(cycles);
    P1OUT &= ~BIT4;
    _clock_out_payload();
    ++nctrl.cur_frame.cur_timeslot;
}

void rx_packet_received()
{
    u8 next_ts = 0;
    rtc_stop();
    rtc_set_cb(0);
    radio_disable();

    if (!rx_synced)
    {
        rx_synced = 1;
        toggel_pin_next_isr = 1;
        rtc_set_mode(RTC_MODE_REPEAT);
        rtc_set_interrupt_tick_count(0);
    }

    if (!rx_frame_synced)
    {
        rx_frame_synced = 1;
        _set_short_listen();
    }

    if (nctrl.cur_frame.our_timeslot)
        _set_timeslot_bit();

    next_ts = nctrl.cur_frame.cur_timeslot + 1;
    if (nctrl.cur_frame.our_timeslot && next_ts == nctrl.cur_frame.our_timeslot) // TX
    {
        _prepare_next_timeslot(nctrl.t.rx_packet_to_tx);
        _rx_end_next_tx();
    }
    else if (next_ts > nctrl.timeslots_per_frame) // END Frame
    {
        _prepare_next_timeslot(nctrl.t.timeslot);
        _rx_end_next_frame_end();
    }
    else // RX
    {
        _prepare_next_timeslot(nctrl.t.rx_packet_to_rx);
        _rx_end_next_rx();
    }

#ifdef DBC_UART_DEBUG
    bc_print_int(nctrl.cur_frame.cur_timeslot - 1, 10);
    bc_print(" RX\n\r");
#ifdef RADIO_DEBUG_RX_PACKET
    bc_print("fi: ");
    bc_print_byte(pckt.frame_ind, 10);
    bc_print(" ts: ");
    bc_print_byte(pckt.timeslot, 10);
    bc_print(" tsm: ");
    bc_print_int(pckt.timeslot_mask, 10);
    bc_print(" nc: ");
    bc_print_byte(pckt.total_node_count, 10);
    bc_print(" f1: ");
    bc_print_byte(pckt.future1, 10);
    bc_print(" f2: ");
    bc_print_byte(pckt.future2, 10);
    bc_print(" sad: ");
    bc_print_byte(pckt.data.src_addr, 10);
    bc_print(" dad: ");
    bc_print_byte(pckt.data.dest_addr, 10);
    bc_print(" dat: ");
    bc_print_int(pckt.data.data, 10);
    for (int i = 0; i < MAX_NODE_HOPS; ++i)
    {
        bc_print("\r\nfwd_src: ");
        bc_print_byte(pckt.fwd[i].src_addr, 10);
        bc_print(" fwd_dst: ");
        bc_print_byte(pckt.fwd[i].dest_addr, 10);
        bc_print(" fwd_data: ");
        bc_print_int(pckt.fwd[i].data, 10);
    }
#endif
#endif
}

void frame_rx_timeslot_end()
{
    P1OUT &= ~BIT4;
    radio_disable();

    // Remove this timeslot from our timeslot mask entry
    if (nctrl.cur_frame.our_timeslot)
        _unset_timeslot_bit();

    if (CUR_TIMESLOT_DATA.timeslot_mask)
        ++CUR_TIMESLOT_DATA.no_rx_count;

    if (CUR_TIMESLOT_DATA.no_rx_count >= NO_RX_COUNT && !nctrl.cur_frame.remove_next_frame && !nctrl.cur_frame.remove_this_frame)
        nctrl.cur_frame.remove_next_frame = CUR_TIMESLOT_DATA.data.src_addr;

    // Increment to next timeslot
    ++nctrl.cur_frame.cur_timeslot;
    if (nctrl.cur_frame.cur_timeslot == nctrl.cur_frame.our_timeslot) // TX
        _rx_end_next_tx();
    else if (nctrl.cur_frame.cur_timeslot > nctrl.timeslots_per_frame) // END Frame
        _rx_end_next_frame_end();
    else // RX
        _rx_end_next_rx();
}

void frame_rx_timeslot_begin()
{
    // Turn the RX on - should already be configured for RX
    P1OUT |= BIT4;
    radio_enable();

    // Set the callback to the end of the timeslot assuming we receive no packet
    rtc_set_cb(frame_rx_timeslot_end);

    // Set the timer for after the callback..
    // If we receive a packet, we are gonna restart the timer
    if ((nctrl.cur_frame.cur_timeslot + 1) == nctrl.cur_frame.our_timeslot)
        rtc_set_tick_cycles(nctrl.t.rx_no_packet_to_tx);
    else if ((nctrl.cur_frame.cur_timeslot + 1) > nctrl.timeslots_per_frame)
        rtc_set_tick_cycles(nctrl.t.rx_no_packet_to_end_frame);
    else
        rtc_set_tick_cycles(nctrl.t.rx_no_packet_to_rx);
}

void frame_tx_timeslot()
{
    P1OUT |= BIT4;
    radio_enable_pulse();

    // Increment to next timeslot
    ++nctrl.cur_frame.cur_timeslot;

    // The next timeslot will either be RX or end of frame
    if (nctrl.cur_frame.cur_timeslot <= nctrl.timeslots_per_frame)
    {
        // Next timeslot will be RX timeslot - switch to RX mode
        // Radio is configured as RX in TX callback func (when packet is done being sent)

        // Set the callback that should be called next - the time counting to this callback is currently ticking
        rtc_set_cb(frame_rx_timeslot_begin);

        // Set the time for AFTER the above callback is called - so the RTC will transfer to the new time
        // We know the next thing is to turn the RX on
        rtc_set_tick_cycles(nctrl.t.rx_on);
    }
    else
    {
        rtc_set_cb(frame_end);
        rtc_set_tick_cycles(nctrl.t.timeslot);
    }
}

void turn_rx_off()
{
    if (!rx_synced)
    {
        P1OUT ^= BIT5;
        rx_synced = 1;

        // Setup TX root node packet
        nctrl.cur_frame.ind = 0;
        nctrl.cur_frame.our_timeslot = 1;
        nctrl.cur_frame.cur_timeslot = 1;
        nctrl.total_node_count = 1;
        OUR_TIMESLOT_DATA.timeslot_mask = 1;
        OUR_TIMESLOT_DATA.data.src_addr = 0x01;
        OUR_TIMESLOT_DATA.data.dest_addr = 0x00;

#ifdef BC_UART_DEBUG
        bc_print_crlf("No SYNC TX");
#endif
        radio_disable();
        radio_configure(RADIO_TX);
        _clock_in_payload();

        rtc_set_mode(RTC_MODE_REPEAT);
        rtc_set_tick_cycles(nctrl.t.tx_to_rx);
        rtc_set_interrupt_tick_count(0);
        rtc_set_cb(0);
        rtc_start();
        rtc_reset();
        while (rtc_get_elapsed() != 1)
            ;
        frame_tx_timeslot();
    }
}

static void _set_short_listen()
{
    nctrl.src_t.listen = nctrl.src_t.packet_listen + nctrl.src_t.drift_listen;
    _recalc_rtc_derived_from_source();
}

static void _set_long_listen()
{
    nctrl.src_t.listen = nctrl.src_t.packet_listen + nctrl.src_t.drift_listen + nctrl.src_t.frame_extra_drift_listen;
    _recalc_rtc_derived_from_source();
}

static void _recalc_rtc_derived_from_source()
{
    nctrl.t.rx_on = (nctrl.src_t.tx_to_rx_measured_delay + 2 * nctrl.src_t.listen) - 1;                                       //
    nctrl.t.rx_no_packet_to_tx = (nctrl.src_t.timeslot - (nctrl.src_t.tx_to_rx_measured_delay + nctrl.src_t.listen)) - 1;     //
    nctrl.t.rx_no_packet_to_rx = (nctrl.src_t.timeslot - (nctrl.src_t.tx_to_rx_measured_delay + 2 * nctrl.src_t.listen)) - 1; //
    nctrl.t.rx_no_packet_to_end_frame = (nctrl.src_t.timeslot - nctrl.src_t.listen) - 1;                                      //
    nctrl.t.rx_packet_to_tx = (nctrl.src_t.timeslot - nctrl.src_t.tx_to_rx_measured_delay) - 1;                               //
    nctrl.t.rx_packet_to_rx = (nctrl.src_t.timeslot - (nctrl.src_t.tx_to_rx_measured_delay + nctrl.src_t.listen)) - 1;        //
    nctrl.t.tx_to_rx = (nctrl.src_t.timeslot - nctrl.src_t.listen) - 1;                                                       //
    nctrl.t.tx_to_end_frame = (nctrl.src_t.timeslot + nctrl.src_t.tx_to_rx_measured_delay) - 1;                               //
    nctrl.t.timeslot = nctrl.src_t.timeslot - 1;                                                                              //
}

static void _setup_rtc()
{
    // Setup control parameters
    nctrl.timeslots_per_frame = 8;
    nctrl.sleep_frame_count = 6;
    nctrl.startup_listen_frame_count = nctrl.sleep_frame_count + 1; // Wait 4 * 2 == 8 seconds

    nctrl.src_t.timeslot = CRYSTAL_FREQ / 8;  // 125 ms
    nctrl.src_t.tx_to_rx_measured_delay = 15; // DONT FREAKING CHANGE THIS NUMBER

    double exact_calc = CRYSTAL_PPM * 0.000001 * nctrl.src_t.timeslot * nctrl.timeslots_per_frame;
    double pckt_oa = ((double)(PACKET_BYTE_SIZE * 8) / 2000000) * CRYSTAL_FREQ;
    nctrl.src_t.drift_listen = (u8)(exact_calc + ROUND_THRESHOLD);
    nctrl.src_t.frame_extra_drift_listen = (u8)(exact_calc * nctrl.sleep_frame_count + ROUND_THRESHOLD) - nctrl.src_t.drift_listen;
    nctrl.src_t.packet_listen = (u8)(pckt_oa / 2.0 + ROUND_THRESHOLD);
    _set_long_listen();

    rtc_init();
    rtc_set_interrupt_tick_count(nctrl.timeslots_per_frame * nctrl.startup_listen_frame_count);
    rtc_set_tick_cycles(nctrl.t.timeslot);
    rtc_set_mode(RTC_MODE_ONE_SHOT);
    rtc_set_cb(turn_rx_off);
}
#endif

void node_control_init()
{
    _setup_clocks();
    _setup_pins();
    bc_init();
    radio_init();

#ifndef RADIO_DEBUG_SPI
    _setup_rtc();
    radio_set_pckt_rx_cb(rx_packet_received);
    radio_set_pckt_tx_cb(tx_packet_sent);
#endif // !RADIO_DEBUG_SPI
    soil_sensor_init();
    soil_sensor_set_cb(_sample_callback);
    _EINT();

#ifdef DBC_UART_DEBUG
    bc_print("Ts, Tl, Tdl, Tlframe, Tpl, Tmeas\n\r");
    bc_print_int(nctrl.src_t.timeslot, 10);
    bc_print_raw(',');
    bc_print_byte(nctrl.src_t.listen, 10);
    bc_print_raw(',');
    bc_print_byte(nctrl.src_t.drift_listen, 10);
    bc_print_raw(',');
    bc_print_byte(nctrl.src_t.frame_extra_drift_listen, 10);
    bc_print_raw(',');
    bc_print_byte(nctrl.src_t.packet_listen, 10);
    bc_print_raw(',');
    bc_print_byte(nctrl.src_t.tx_to_rx_measured_delay, 10);
    bc_print("\n\rTime delays...\n\rRXNP->TX:");
    bc_print_int(nctrl.t.rx_no_packet_to_tx, 10);
    bc_print("\n\rRXNP->RX:");
    bc_print_int(nctrl.t.rx_no_packet_to_rx, 10);
    bc_print("\n\rRXNP->EF:");
    bc_print_int(nctrl.t.rx_no_packet_to_end_frame, 10);
    bc_print("\n\rRXP->TX:");
    bc_print_int(nctrl.t.rx_packet_to_tx, 10);
    bc_print("\n\rRXP->RX:");
    bc_print_int(nctrl.t.rx_packet_to_rx, 10);
    bc_print("\n\rTX->RX:");
    bc_print_int(nctrl.t.tx_to_rx, 10);
    bc_print("\n\rTX->EF:");
    bc_print_int(nctrl.t.tx_to_end_frame, 10);
    bc_print("\n\rRX_ON:");
    bc_print_int(nctrl.t.rx_on, 10);
    bc_print("\n\r");
    bc_print_crlf("\n\rInitialized");
#endif
}

void node_control_run()
{
#ifndef RADIO_DEBUG_SPI
    radio_configure(RADIO_RX);
    radio_enable();
    rtc_start();
#endif // !RADIO_DEBUG_SPI
    while (1)
    {
        rtc_update();
        bc_update();
        radio_update();
        soil_sensor_update();
        LPM4;
    }
}

void node_control_shutdown()
{}

void _setup_pins()
{
    // ACLK on P8.1
    P8SEL0 |= BIT1;
    SYSCFG2 &= ~ADCPCTL9;
    P8DIR |= BIT1;

    // Use P1.5 to show Frame start and end
    SYSCFG2 &= ~ADCPCTL5;
    P1SEL0 &= ~BIT5;
    P1DIR |= BIT5;
    P1OUT &= ~BIT5;

    // Use P1.4 to show timeslot activity
    SYSCFG2 &= ~ADCPCTL4;
    P1SEL0 &= ~BIT4;
    P1DIR |= BIT4;
    P1OUT &= ~BIT4;
}

void _setup_clocks()
{
    WDTCTL = WDTPW | WDTHOLD; // Stop watchdog timer

    // This should really be after
    PM5CTL0 &= ~LOCKLPM5;

    /*
    This section is to get the DCO clock set correctly quickly, so going in to LPM doesn't prevent it from getting FLL lock
    */
    // Clear DCO and OFIFG flags before disabling FLL
    CSCTL7 &= ~DCOFFG;
    SFRIFG1 &= ~OFIFG;

    // Turn off the FLL
    __bis_SR_register(SCG0);

    // Select REFO as source clock
    CSCTL3 &= ~(SELREF0 | SELREF1);
    CSCTL3 |= SELREF_1;

    // Clear the DCO and MOD registers
    CSCTL0 = 0;

    // Unset all dcorsel bits, then set for 16 MHz range
    CSCTL1 &= ~(DCORSEL0 | DCORSEL1 | DCORSEL2);
    CSCTL1 |= DCORSEL_5;

    // We want FLLD at 1 (16 MHz operation) and FLLN at 487
    // This produces DCOCLK and DCOCLKDIV of (FLLN + 1)(REFO) = (487 + 1)(32768) = 15.990784 MHz
    CSCTL2 = (FLLD_0 | 0x01E7);

    // We don't need so high for SMCLK - divide by 4.. would divide by 8 but baud rate doesn't work
    CSCTL5 |= DIVS_2;

    // A few no ops to make the above take affect
    _no_operation();
    _no_operation();
    _no_operation();

    // Re-enable FLL
    __bic_SR_register(SCG0);

    // Poll until the FLL has reached a lock!
    while ((CSCTL7 & (FLLUNLOCK0 | FLLUNLOCK1)) || (CSCTL7 & DCOFFG))
    {
        // Clear OSC fault flags
        CSCTL7 &= ~DCOFFG;

        // Clear OFIFG fault flag
        SFRIFG1 &= ~OFIFG;
    }

    // Setup XT1 for crystal
    P4SEL0 |= BIT1;
    P4SEL0 |= BIT2;

    CSCTL4 &= ~0x0F00;
}
