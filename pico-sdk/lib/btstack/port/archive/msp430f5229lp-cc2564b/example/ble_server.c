/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */


// *****************************************************************************
//
// att device demo
//
// *****************************************************************************

// TODO: seperate BR/EDR from LE ACL buffers
// TODO: move LE init into HCI
// ..

// NOTE: Supports only a single connection

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_config.h"

#include <msp430x54x.h>

#include "btstack_chipset_cc256x.h"
#include "hal_board.h"
#include "hal_compat.h"
#include "hal_usb.h"
#include "hal_usb.h"

#include "btstack.h"


#define FONT_HEIGHT		12                    // Each character has 13 lines 
#define FONT_WIDTH       8
#define MAX_CHR01_VALUE_LENGTH 40

static uint16_t chr01_value_length = 0;
static char chr01_value[MAX_CHR01_VALUE_LENGTH];
static char chr02_value = 0;

// enable LE, setup ADV data
static void app_packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (packet_type != HCI_EVENT_PACKET) return;
    bd_addr_t addr;
    uint8_t adv_data[] = { 02, 01, 05,   03, 02, 0xf0, 0xff }; 
    
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            // bt stack activated, get started - set local name
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING){
                printf("Working!\n");
                hci_send_cmd(&hci_le_set_advertising_data, sizeof(adv_data), adv_data);
            }
            break;
            
        case BTSTACK_EVENT_NR_CONNECTIONS_CHANGED:
            if (packet[2]) {
                printf("CONNECTED");
            } else {
                printf("DISCONNECTED");
            }
            break;
            
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            // restart advertising
            hci_send_cmd(&hci_le_set_advertise_enable, 1);
            break;
            
        case HCI_EVENT_COMMAND_COMPLETE:
            if (hci_event_command_complete_get_command_opcode(packet) == HCI_OPCODE_HCI_READ_BD_ADDR) {
                reverse_bd_addr(&packet[6], addr);
                printf("BD ADDR: %s\n", bd_addr_to_str(addr));
                break;
            }
            if (hci_event_command_complete_get_command_opcode(packet) == HCI_HCI_OPCODE_HCI_LE_SET_ADVERTISING_DATA) {
               hci_send_cmd(&hci_le_set_scan_response_data, 10, adv_data);
               break;
            }
            if (hci_event_command_complete_get_command_opcode(packet) == HCI_HCI_OPCODE_HCI_LE_SET_SCAN_RESPONSE_DATA) {
               hci_send_cmd(&hci_le_set_advertise_enable, 1);
               break;
            }
        default:
            break;
    }
	
}

// test profile
#include "profile.h"

static uint16_t get_read_att_value_len(uint16_t att_handle){
    uint16_t value_len;
    switch(att_handle){
        case ATT_CHARACTERISTIC_FFF1_01_VALUE_HANDLE:
            value_len = chr01_value_length;
            break;
        case ATT_CHARACTERISTIC_FFF2_01_VALUE_HANDLE:
            value_len = 1;
            break;
        default:
            value_len = 0;
            break;
    }
    return value_len;
}

static uint16_t get_write_att_value_len(uint16_t att_handle){
    uint16_t value_len;
    switch(att_handle){
        case ATT_CHARACTERISTIC_FFF1_01_VALUE_HANDLE:
            value_len = MAX_CHR01_VALUE_LENGTH;
            break;
        case ATT_CHARACTERISTIC_FFF2_01_VALUE_HANDLE:
            value_len = 1;
            break;
        default:
            value_len = 0;
            break;
    }
    return value_len;
}

static uint16_t get_bytes_to_copy(uint16_t value_len, uint16_t offset, uint16_t buffer_size){
    if (value_len <= offset ) return 0;
    
    uint16_t bytes_to_copy = value_len - offset;
    if (bytes_to_copy > buffer_size) {
        bytes_to_copy = buffer_size;
    }
    return bytes_to_copy;
}

uint16_t att_read_callback(hci_con_handle_t con_handle, uint16_t att_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size){
    printf("READ Callback, handle %04x\n", att_handle);
    uint16_t value_len = get_read_att_value_len(att_handle);
    if (!buffer) return value_len;
    
    uint16_t bytes_to_copy = get_bytes_to_copy(value_len, offset, buffer_size);
    if (!bytes_to_copy) return 0;
    
    switch(att_handle){
        case ATT_CHARACTERISTIC_FFF1_01_VALUE_HANDLE:
            memcpy(buffer, &chr01_value[offset], bytes_to_copy);
            break;
        case ATT_CHARACTERISTIC_FFF2_01_VALUE_HANDLE:
            buffer[offset] = chr02_value;
            break;
    }
    return bytes_to_copy;
}

// write requests
static int att_write_callback(hci_con_handle_t con_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size){
    printf("WRITE Callback, handle %04x\n", att_handle);
    
    uint16_t value_len = get_write_att_value_len(att_handle);
    uint16_t bytes_to_copy = get_bytes_to_copy(value_len, offset,buffer_size);
    if (!bytes_to_copy) return ATT_ERROR_INVALID_OFFSET;
    
    switch(att_handle){
        case ATT_CHARACTERISTIC_FFF1_01_VALUE_HANDLE:
            buffer[buffer_size] = 0;
            memcpy(&chr01_value[offset], buffer, bytes_to_copy);
            chr01_value_length = bytes_to_copy + offset;
            
            printf("New text: %s\n", buffer);
            break;
        case ATT_CHARACTERISTIC_FFF2_01_VALUE_HANDLE:
            printf("New value: %u\n", buffer[offset]);
#if 0
            if (buffer[offset]) {
                LED_PORT_OUT |= LED_2;
            } else {
                LED_PORT_OUT &= ~LED_2;
            }
#endif
            chr02_value = buffer[offset];
            break;
    }
    return 0;
}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){

    // set up l2cap_le
    l2cap_init();
    
    // setup le device db
    le_device_db_init();

    // setup SM: Display only
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements( SM_AUTHREQ_BONDING | SM_AUTHREQ_MITM_PROTECTION); 

    // setup ATT server
    att_server_init(profile_data, NULL, att_write_callback);    
    att_server_register_packet_handler(app_packet_handler);
    
	printf("Run...\n\r");

    // turn on!
	hci_power_control(HCI_POWER_ON);

    // LED_PORT_OUT &= ~LED_2;

    return 0;
}

