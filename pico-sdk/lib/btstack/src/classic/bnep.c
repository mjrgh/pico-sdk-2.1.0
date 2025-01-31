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

#define BTSTACK_FILE__ "bnep.c"

/*
 * bnep.c
 * Author: Ole Reinhardt <ole.reinhardt@kernelconcepts.de>
 *
 */

#include <stdint.h>
#include <string.h> // memcpy

#include "bluetooth_psm.h"
#include "bluetooth_sdp.h"
#include "bnep.h"
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_util.h"
#include "classic/core.h"
#include "classic/sdp_util.h"
#include "hci.h"
#include "hci_cmd.h"
#include "hci_dump.h"
#include "l2cap.h"

#define BNEP_EXT_FLAG                                   0x80
#define BNEP_TYPE_MASK                                  0x7F
#define BNEP_TYPE(header)                               ((header) & BNEP_TYPE_MASK)
#define BNEP_HEADER_HAS_EXT(x)                          (((x) & BNEP_EXT_FLAG) == BNEP_EXT_FLAG)
    
/* BNEP packet types */    
#define BNEP_PKT_TYPE_GENERAL_ETHERNET                  0x00
#define BNEP_PKT_TYPE_CONTROL                           0x01
#define BNEP_PKT_TYPE_COMPRESSED_ETHERNET               0x02
#define BNEP_PKT_TYPE_COMPRESSED_ETHERNET_SOURCE_ONLY   0x03
#define BNEP_PKT_TYPE_COMPRESSED_ETHERNET_DEST_ONLY     0x04

/* BNEP control types */
#define BNEP_CONTROL_TYPE_COMMAND_NOT_UNDERSTOOD        0x00
#define BNEP_CONTROL_TYPE_SETUP_CONNECTION_REQUEST      0x01
#define BNEP_CONTROL_TYPE_SETUP_CONNECTION_RESPONSE     0x02
#define BNEP_CONTROL_TYPE_FILTER_NET_TYPE_SET           0x03
#define BNEP_CONTROL_TYPE_FILTER_NET_TYPE_RESPONSE      0x04
#define BNEP_CONTROL_TYPE_FILTER_MULTI_ADDR_SET         0x05
#define BNEP_CONTROL_TYPE_FILTER_MULTI_ADDR_RESPONSE    0x06

/* BNEP extension header types */
#define BNEP_EXT_HEADER_TYPE_EXTENSION_CONTROL          0x00

/* BNEP setup response codes */
#define BNEP_RESP_SETUP_SUCCESS                         0x0000
#define BNEP_RESP_SETUP_INVALID_DEST_UUID               0x0001
#define BNEP_RESP_SETUP_INVALID_SOURCE_UUID             0x0002
#define BNEP_RESP_SETUP_INVALID_SERVICE_UUID_SIZE       0x0003
#define BNEP_RESP_SETUP_CONNECTION_NOT_ALLOWED          0x0004

/* BNEP filter response codes */
#define BNEP_RESP_FILTER_SUCCESS                        0x0000
#define BNEP_RESP_FILTER_UNSUPPORTED_REQUEST            0x0001
#define BNEP_RESP_FILTER_ERR_INVALID_RANGE              0x0002
#define BNEP_RESP_FILTER_ERR_TOO_MANY_FILTERS           0x0003
#define BNEP_RESP_FILTER_ERR_SECURITY                   0x0004

#define BNEP_CONNECTION_TIMEOUT_MS 10000
#define BNEP_CONNECTION_MAX_RETRIES 1

static btstack_linked_list_t bnep_services = NULL;
static btstack_linked_list_t bnep_channels = NULL;

static gap_security_level_t bnep_security_level;

static bnep_channel_t * bnep_channel_for_l2cap_cid(uint16_t l2cap_cid);
static void bnep_channel_finalize(bnep_channel_t *channel);
static void bnep_channel_start_timer(bnep_channel_t *channel, int timeout);
inline static void bnep_channel_state_add(bnep_channel_t *channel, BNEP_CHANNEL_STATE_VAR event);
static void bnep_handle_can_send_now(uint16_t cid);
static void bnep_emit_open_channel_complete(bnep_channel_t *channel, uint8_t status) 
{
    log_info("BNEP_EVENT_CHANNEL_OPENED status 0x%02x bd_addr: %s, handler %p", status, bd_addr_to_str(channel->remote_addr), channel->packet_handler);
    if (!channel->packet_handler) return;

    uint8_t event[3 + sizeof(bd_addr_t) + 4 * sizeof(uint16_t) + 2];
    event[0] = BNEP_EVENT_CHANNEL_OPENED;
    event[1] = sizeof(event) - 2;
    event[2] = status;
    little_endian_store_16(event, 3, channel->l2cap_cid);
    little_endian_store_16(event, 5, channel->uuid_source);
    little_endian_store_16(event, 7, channel->uuid_dest);
    little_endian_store_16(event, 9, channel->max_frame_size);
    reverse_bd_addr(channel->remote_addr, &event[11]);
    little_endian_store_16(event, 17, channel->con_handle);
    hci_dump_btstack_event( event, sizeof(event));
	(*channel->packet_handler)(HCI_EVENT_PACKET, 0, (uint8_t *) event, sizeof(event));
}

static void bnep_emit_channel_timeout(bnep_channel_t *channel) 
{
    log_info("BNEP_EVENT_CHANNEL_TIMEOUT bd_addr: %s, handler %p", bd_addr_to_str(channel->remote_addr), channel->packet_handler);
    if (!channel->packet_handler) return;

    uint8_t event[2 + sizeof(bd_addr_t) + 3 * sizeof(uint16_t) + sizeof(uint8_t)];
    event[0] = BNEP_EVENT_CHANNEL_TIMEOUT;
    event[1] = sizeof(event) - 2;
    little_endian_store_16(event, 2, channel->l2cap_cid);
    little_endian_store_16(event, 4, channel->uuid_source);
    little_endian_store_16(event, 6, channel->uuid_dest);
    reverse_bd_addr(channel->remote_addr, &event[8]);
    event[14] = channel->state; 
    hci_dump_btstack_event( event, sizeof(event));
	(*channel->packet_handler)(HCI_EVENT_PACKET, 0, (uint8_t *) event, sizeof(event));
}

static void bnep_emit_channel_closed(bnep_channel_t *channel) 
{
    log_info("BNEP_EVENT_CHANNEL_CLOSED bd_addr: %s, handler %p", bd_addr_to_str(channel->remote_addr), channel->packet_handler);
    if (!channel->packet_handler) return;

    uint8_t event[2 + sizeof(bd_addr_t) + 3 * sizeof(uint16_t)];
    event[0] = BNEP_EVENT_CHANNEL_CLOSED;
    event[1] = sizeof(event) - 2;
    little_endian_store_16(event, 2, channel->l2cap_cid);
    little_endian_store_16(event, 4, channel->uuid_source);
    little_endian_store_16(event, 6, channel->uuid_dest);
    reverse_bd_addr(channel->remote_addr, &event[8]);
    hci_dump_btstack_event( event, sizeof(event));
	(*channel->packet_handler)(HCI_EVENT_PACKET, 0, (uint8_t *) event, sizeof(event));
}

static void bnep_emit_ready_to_send(bnep_channel_t *channel)
{
    if (!channel->packet_handler) return;

    uint8_t event[4];
    event[0] = BNEP_EVENT_CAN_SEND_NOW;
    event[1] = sizeof(event) - 2;
    little_endian_store_16(event, 2, channel->l2cap_cid);
    hci_dump_btstack_event( event, sizeof(event));
	(*channel->packet_handler)(HCI_EVENT_PACKET, 0, (uint8_t *) event, sizeof(event));
}

/* Send BNEP connection request */
static int bnep_send_command_not_understood(bnep_channel_t *channel, uint8_t control_type)
{
    uint8_t *bnep_out_buffer = NULL;
    uint16_t pos = 0;
    int      err = 0; 
    
    if (channel->state == BNEP_CHANNEL_STATE_CLOSED) {
        return -1; // TODO
    }
       
    l2cap_reserve_packet_buffer();
    bnep_out_buffer = l2cap_get_outgoing_buffer();

    /* Setup control packet type */
	bnep_out_buffer[pos++] = BNEP_PKT_TYPE_CONTROL;
	bnep_out_buffer[pos++] = BNEP_CONTROL_TYPE_COMMAND_NOT_UNDERSTOOD;

    /* Add not understood control type */
    bnep_out_buffer[pos++] = control_type;    

    err = l2cap_send_prepared(channel->l2cap_cid, pos);
    
    if (err) {
        // TODO: Log error 
    }
    return err;
}


/* Send BNEP connection request */
static int bnep_send_connection_request(bnep_channel_t *channel, uint16_t uuid_source, uint16_t uuid_dest)
{
    uint8_t *bnep_out_buffer = NULL;
    uint16_t pos = 0;
    int      err = 0; 
    
    if (channel->state == BNEP_CHANNEL_STATE_CLOSED) {
        return -1; // TODO
    }
    
    l2cap_reserve_packet_buffer();
    bnep_out_buffer = l2cap_get_outgoing_buffer();

    /* Setup control packet type */
	bnep_out_buffer[pos++] = BNEP_PKT_TYPE_CONTROL;
	bnep_out_buffer[pos++] = BNEP_CONTROL_TYPE_SETUP_CONNECTION_REQUEST;

    /* Add UUID Size */
    bnep_out_buffer[pos++] = 2;

    /* Add dest and source UUID */
    big_endian_store_16(bnep_out_buffer, pos, uuid_dest);
    pos += 2;

    big_endian_store_16(bnep_out_buffer, pos, uuid_source);
    pos += 2;

    err = l2cap_send_prepared(channel->l2cap_cid, pos);
    
    if (err) {
        // TODO: Log error 
    }
    return err;
}

/* Send BNEP connection response */
static int bnep_send_connection_response(bnep_channel_t *channel, uint16_t response_code)
{
    uint8_t *bnep_out_buffer = NULL;
    uint16_t pos = 0;
    int      err = 0; 
    
    if (channel->state == BNEP_CHANNEL_STATE_CLOSED) {
        return -1; // TODO
    }
    
    l2cap_reserve_packet_buffer();
    bnep_out_buffer = l2cap_get_outgoing_buffer();

    /* Setup control packet type */
	bnep_out_buffer[pos++] = BNEP_PKT_TYPE_CONTROL;
	bnep_out_buffer[pos++] = BNEP_CONTROL_TYPE_SETUP_CONNECTION_RESPONSE;

    /* Add response code */
    big_endian_store_16(bnep_out_buffer, pos, response_code);
    pos += 2;

    err = l2cap_send_prepared(channel->l2cap_cid, pos);
    
    if (err) {
        // TODO: Log error 
    }
    return err;
}

/* Send BNEP filter net type set message */
static int bnep_send_filter_net_type_set(bnep_channel_t *channel, bnep_net_filter_t *filter, uint16_t len)
{  
    uint8_t *bnep_out_buffer = NULL;
    uint16_t pos = 0;
    int      err = 0; 
    int      i;

    if (channel->state == BNEP_CHANNEL_STATE_CLOSED) {
        return -1;
    }

    l2cap_reserve_packet_buffer();
    bnep_out_buffer = l2cap_get_outgoing_buffer();

    /* Setup control packet type */
	bnep_out_buffer[pos++] = BNEP_PKT_TYPE_CONTROL;
	bnep_out_buffer[pos++] = BNEP_CONTROL_TYPE_FILTER_NET_TYPE_SET;

    big_endian_store_16(bnep_out_buffer, pos, len * 2 * 2);
    pos += 2;

    for (i = 0; i < len; i ++) {
        big_endian_store_16(bnep_out_buffer, pos, filter[i].range_start);
        pos += 2;
        big_endian_store_16(bnep_out_buffer, pos, filter[i].range_end);
        pos += 2;
    }

    err = l2cap_send_prepared(channel->l2cap_cid, pos);
    
    if (err) {
        // TODO: Log error 
    }
    return err;
}

/* Send BNEP filter net type response message */
static int bnep_send_filter_net_type_response(bnep_channel_t *channel, uint16_t response_code)
{
    uint8_t *bnep_out_buffer = NULL;
    uint16_t pos = 0;
    int      err = 0; 
    
    if (channel->state == BNEP_CHANNEL_STATE_CLOSED) {
        return -1;
    }
    
    l2cap_reserve_packet_buffer();
    bnep_out_buffer = l2cap_get_outgoing_buffer();

    /* Setup control packet type */
	bnep_out_buffer[pos++] = BNEP_PKT_TYPE_CONTROL;
	bnep_out_buffer[pos++] = BNEP_CONTROL_TYPE_FILTER_NET_TYPE_RESPONSE;

    /* Add response code */
    big_endian_store_16(bnep_out_buffer, pos, response_code);
    pos += 2;

    err = l2cap_send_prepared(channel->l2cap_cid, pos);
    
    if (err) {
        // TODO: Log error 
    }
    return err;
}

/* Send BNEP filter multicast address set message */

static int bnep_send_filter_multi_addr_set(bnep_channel_t *channel, bnep_multi_filter_t *filter, uint16_t len)
{
    uint8_t *bnep_out_buffer = NULL;
    uint16_t pos = 0;
    int      err = 0; 
    int      i;

    if (channel->state == BNEP_CHANNEL_STATE_CLOSED) {
        return -1;
    }

    l2cap_reserve_packet_buffer();
    bnep_out_buffer = l2cap_get_outgoing_buffer();

    /* Setup control packet type */
	bnep_out_buffer[pos++] = BNEP_PKT_TYPE_CONTROL;
	bnep_out_buffer[pos++] = BNEP_CONTROL_TYPE_FILTER_MULTI_ADDR_SET;

    big_endian_store_16(bnep_out_buffer, pos, len * 2 * ETHER_ADDR_LEN);
    pos += 2;

    for (i = 0; i < len; i ++) {
        bd_addr_copy(bnep_out_buffer + pos, filter[i].addr_start);
        pos += ETHER_ADDR_LEN;
        bd_addr_copy(bnep_out_buffer + pos, filter[i].addr_end);
        pos += ETHER_ADDR_LEN;
    }

    err = l2cap_send_prepared(channel->l2cap_cid, pos);
    
    if (err) {
        // TODO: Log error 
    }
    return err;
}

/* Send BNEP filter multicast address response message */
static int bnep_send_filter_multi_addr_response(bnep_channel_t *channel, uint16_t response_code)
{
    uint8_t *bnep_out_buffer = NULL;
    uint16_t pos = 0;
    int      err = 0; 
    
    if (channel->state == BNEP_CHANNEL_STATE_CLOSED) {
        return -1;
    }
    
    l2cap_reserve_packet_buffer();
    bnep_out_buffer = l2cap_get_outgoing_buffer();

    /* Setup control packet type */
	bnep_out_buffer[pos++] = BNEP_PKT_TYPE_CONTROL;
	bnep_out_buffer[pos++] = BNEP_CONTROL_TYPE_FILTER_MULTI_ADDR_RESPONSE;

    /* Add response code */
    big_endian_store_16(bnep_out_buffer, pos, response_code);
    pos += 2;

    err = l2cap_send_prepared(channel->l2cap_cid, pos);
    
    if (err) {
        // TODO: Log error 
    }
    return err;
}

int bnep_can_send_packet_now(uint16_t bnep_cid)
{
    bnep_channel_t *channel = bnep_channel_for_l2cap_cid(bnep_cid);

    if (!channel){
        log_error("bnep_can_send_packet_now cid 0x%02x doesn't exist!", bnep_cid);
        return 0;
    }
    
    return l2cap_can_send_packet_now(channel->l2cap_cid);
}

void bnep_request_can_send_now_event(uint16_t bnep_cid)
{
    bnep_channel_t *channel = bnep_channel_for_l2cap_cid(bnep_cid);

    if (!channel){
        log_error("bnep_request_can_send_now_event cid 0x%02x doesn't exist!", bnep_cid);
        return;
    }

    channel->waiting_for_can_send_now = 1;
    l2cap_request_can_send_now_event(bnep_cid);
}


static int bnep_filter_protocol(bnep_channel_t *channel, uint16_t network_protocol_type)
{
	int i;
    
    if (channel->net_filter_count == 0) {
        /* No filter set */
        return 1;
    }

    for (i = 0; i < channel->net_filter_count; i ++) {
        if ((network_protocol_type >= channel->net_filter[i].range_start) &&
            (network_protocol_type <= channel->net_filter[i].range_end)) {
            return 1;
        }
    }

    return 0;
}

static int bnep_filter_multicast(bnep_channel_t *channel, bd_addr_t addr_dest)
{
	int i;

    /* Check if the multicast flag is set int the destination address */
	if ((addr_dest[0] & 0x01) == 0x00) {
        /* Not a multicast frame, do not apply filtering and send it in any case */
		return 1;
    }

    if (channel->multicast_filter_count == 0) {
        /* No filter set */
        return 1;
    }

	for (i = 0; i < channel->multicast_filter_count; i ++) {
		if ((memcmp(addr_dest, channel->multicast_filter[i].addr_start, sizeof(bd_addr_t)) >= 0) &&
		    (memcmp(addr_dest, channel->multicast_filter[i].addr_end, sizeof(bd_addr_t)) <= 0)) {
			return 1;
        }
	}

	return 0;
}


/* Send BNEP ethernet packet */
int bnep_send(uint16_t bnep_cid, uint8_t *packet, uint16_t len)
{
    bnep_channel_t *channel;
    uint8_t        *bnep_out_buffer = NULL;
    uint16_t        pos = 0;
    uint16_t        pos_out = 0;
    uint16_t        payload_len;
    int             err = 0;
    int             has_source;
    int             has_dest;

    bd_addr_t       addr_dest;
    bd_addr_t       addr_source;
    uint16_t        network_protocol_type;

    channel = bnep_channel_for_l2cap_cid(bnep_cid);
    if (channel == NULL) {
        log_error("bnep_send cid 0x%02x doesn't exist!", bnep_cid);
        return 1;
    }
        
    if (channel->state != BNEP_CHANNEL_STATE_CONNECTED) {
        return BNEP_CHANNEL_NOT_CONNECTED;
    }
    
    /* Check for free ACL buffers */
    if (!l2cap_can_send_packet_now(channel->l2cap_cid)) {
        return BTSTACK_ACL_BUFFERS_FULL;
    }

    /* Extract destination and source address from the ethernet packet */
    pos = 0;
    bd_addr_copy(addr_dest, &packet[pos]);
    pos += sizeof(bd_addr_t);
    bd_addr_copy(addr_source, &packet[pos]);
    pos += sizeof(bd_addr_t);
    network_protocol_type = big_endian_read_16(packet, pos);
    pos += sizeof(uint16_t);

    payload_len = len - pos;

	if (network_protocol_type == ETHERTYPE_VLAN) {	/* IEEE 802.1Q tag header */
		if (payload_len < 4) {
            /* Omit this packet */
			return 0;
        }
        /* The "real" network protocol type is 4 bytes ahead in a VLAN packet */
		network_protocol_type = big_endian_read_16(packet, pos + 2);
	}

    /* Check network protocol and multicast filters before sending */
    if (!bnep_filter_protocol(channel, network_protocol_type) ||
        !bnep_filter_multicast(channel, addr_dest)) {
        /* Packet did not pass filter... */
        if ((network_protocol_type == ETHERTYPE_VLAN) && 
            (payload_len >= 4)) {
            /* The packet has been tagged as a with IEE 802.1Q tag and has been filtered out.
               According to the spec the IEE802.1Q tag header shall be sended without ethernet payload.
               So limit the payload_len to 4.
             */
            payload_len = 4;
        } else {
            /* Packet is not tagged with IEE802.1Q header and was filtered out. Omit this packet */        
            return 0;
        }
    }

    /* Reserve l2cap packet buffer */    
    l2cap_reserve_packet_buffer();
    bnep_out_buffer = l2cap_get_outgoing_buffer();

    /* Check if source address is the same as our local address and if the 
       destination address is the same as the remote addr. Maybe we can use
       the compressed data format
     */ 
    has_source = (memcmp(addr_source, channel->local_addr, ETHER_ADDR_LEN) != 0);
    has_dest = (memcmp(addr_dest, channel->remote_addr, ETHER_ADDR_LEN) != 0);

    /* Check for MTU limits */
    if (payload_len > channel->max_frame_size) {
        log_error("bnep_send: Max frame size (%d) exceeded: %d", channel->max_frame_size, payload_len);
        return BNEP_DATA_LEN_EXCEEDS_MTU;
    }
    
    /* Fill in the package type depending on the given source and destination address */
    if (has_source && has_dest) {
        bnep_out_buffer[pos_out++] = BNEP_PKT_TYPE_GENERAL_ETHERNET;
    } else 
    if (has_source && !has_dest) {
        bnep_out_buffer[pos_out++] = BNEP_PKT_TYPE_COMPRESSED_ETHERNET_SOURCE_ONLY;
    } else 
    if (!has_source && has_dest) {
        bnep_out_buffer[pos_out++] = BNEP_PKT_TYPE_COMPRESSED_ETHERNET_DEST_ONLY;
    } else {
        bnep_out_buffer[pos_out++] = BNEP_PKT_TYPE_COMPRESSED_ETHERNET;
    }

    /* Add the destination address if needed */
    if (has_dest) {
        bd_addr_copy(bnep_out_buffer + pos_out, addr_dest);
        pos_out += sizeof(bd_addr_t);
    }

    /* Add the source address if needed */
    if (has_source) {
        bd_addr_copy(bnep_out_buffer + pos_out, addr_source);
        pos_out += sizeof(bd_addr_t);
    }

    /* Add protocol type */
    big_endian_store_16(bnep_out_buffer, pos_out, network_protocol_type);
    pos_out += 2;
    
    /* TODO: Add extension headers, if we may support them at a later stage */
    /* Add the payload and then send out the package */
    (void)memcpy(bnep_out_buffer + pos_out, packet + pos, payload_len);
    pos_out += payload_len;

    err = l2cap_send_prepared(channel->l2cap_cid, pos_out);
    
    if (err) {
        log_error("bnep_send: error %d", err);
    }
    return err;        
}


/* Set BNEP network protocol type filter */
int bnep_set_net_type_filter(uint16_t bnep_cid, bnep_net_filter_t *filter, uint16_t len)
{
    bnep_channel_t *channel;

    if (filter == NULL) {
        return -1;
    }

    channel = bnep_channel_for_l2cap_cid(bnep_cid);
    if (channel == NULL) {
        log_error("bnep_set_net_type_filter cid 0x%02x doesn't exist!", bnep_cid);
        return 1;
    }
        
    if (channel->state != BNEP_CHANNEL_STATE_CONNECTED) {
        return BNEP_CHANNEL_NOT_CONNECTED;
    }
    
    if (len > MAX_BNEP_NETFILTER_OUT) {
        return BNEP_DATA_LEN_EXCEEDS_MTU;
    }

    channel->net_filter_out = filter;
    channel->net_filter_out_count = len;

    /* Set flag to send out the network protocol type filter set request */
    bnep_channel_state_add(channel, BNEP_CHANNEL_STATE_VAR_SND_FILTER_NET_TYPE_SET);
    l2cap_request_can_send_now_event(channel->l2cap_cid);

    return 0;        
}

/* Set BNEP network protocol type filter */
int bnep_set_multicast_filter(uint16_t bnep_cid,  bnep_multi_filter_t *filter, uint16_t len)
{
    bnep_channel_t *channel;

    if (filter == NULL) {
        return -1;
    }

    channel = bnep_channel_for_l2cap_cid(bnep_cid);
    if (channel == NULL) {
        log_error("bnep_set_net_type_filter cid 0x%02x doesn't exist!", bnep_cid);
        return 1;
    }
        
    if (channel->state != BNEP_CHANNEL_STATE_CONNECTED) {
        return BNEP_CHANNEL_NOT_CONNECTED;
    }
    
    if (len > MAX_BNEP_MULTICAST_FILTER_OUT) {
        return BNEP_DATA_LEN_EXCEEDS_MTU;
    }

    channel->multicast_filter_out = filter;
    channel->multicast_filter_out_count = len;

    /* Set flag to send out the multicast filter set request */
    bnep_channel_state_add(channel, BNEP_CHANNEL_STATE_VAR_SND_FILTER_MULTI_ADDR_SET);
    l2cap_request_can_send_now_event(channel->l2cap_cid);

    return 0;        
}

/* BNEP timeout timer helper function */
static void bnep_channel_timer_handler(btstack_timer_source_t *timer)
{
    bnep_channel_t *channel = btstack_run_loop_get_timer_context(timer);
    // retry send setup connection at least one time
    if (channel->state == BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_RESPONSE){
        if (channel->retry_count < BNEP_CONNECTION_MAX_RETRIES){
            channel->retry_count++;
            bnep_channel_start_timer(channel, BNEP_CONNECTION_TIMEOUT_MS);
            bnep_channel_state_add(channel, BNEP_CHANNEL_STATE_VAR_SND_CONNECTION_REQUEST); 
            l2cap_request_can_send_now_event(channel->l2cap_cid);
            return;
        }
    }

    log_info( "bnep_channel_timeout_handler callback: shutting down connection!");
    bnep_emit_channel_timeout(channel);
    bnep_channel_finalize(channel);
}


static void bnep_channel_stop_timer(bnep_channel_t *channel)
{
    if (channel->timer_active) {
        btstack_run_loop_remove_timer(&channel->timer);
        channel->timer_active = 0;
    }
}

static void bnep_channel_start_timer(bnep_channel_t *channel, int timeout)
{
    /* Stop any eventually running timeout timer */
    bnep_channel_stop_timer(channel);

    /* Start bnep channel timeout check timer */
    btstack_run_loop_set_timer(&channel->timer, timeout);
    btstack_run_loop_set_timer_handler(&channel->timer, bnep_channel_timer_handler);
    btstack_run_loop_set_timer_context(&channel->timer, channel);
    btstack_run_loop_add_timer(&channel->timer);
    channel->timer_active = 1;    
}

/* BNEP statemachine functions */

inline static void bnep_channel_state_add(bnep_channel_t *channel, BNEP_CHANNEL_STATE_VAR event){
    channel->state_var = (BNEP_CHANNEL_STATE_VAR) (channel->state_var | event);    
}
inline static void bnep_channel_state_remove(bnep_channel_t *channel, BNEP_CHANNEL_STATE_VAR event){
    channel->state_var = (BNEP_CHANNEL_STATE_VAR) (channel->state_var & ~event);    
}

static uint16_t bnep_max_frame_size_for_l2cap_mtu(uint16_t l2cap_mtu){

    /* Assume a standard BNEP header, containing BNEP Type (1 Byte), dest and 
       source address (6 bytes each) and networking protocol type (2 bytes)
     */
    uint16_t max_frame_size = l2cap_mtu - 15; // 15 bytes BNEP header
    
    log_info("bnep_max_frame_size_for_l2cap_mtu:  %u -> %u", l2cap_mtu, max_frame_size);
    return max_frame_size;
}

static bnep_channel_t * bnep_channel_create_for_addr(bd_addr_t addr)
{
    /* Allocate new channel structure */
    bnep_channel_t *channel = btstack_memory_bnep_channel_get();
    if (!channel) {
        return NULL;
    }

    channel->state = BNEP_CHANNEL_STATE_CLOSED;
    channel->max_frame_size = bnep_max_frame_size_for_l2cap_mtu(l2cap_max_mtu());
    bd_addr_copy(channel->remote_addr, addr);
    gap_local_bd_addr(channel->local_addr);

    channel->net_filter_count = 0;
    channel->multicast_filter_count = 0;
    channel->retry_count = 0;

    /* Finally add it to the channel list */
    btstack_linked_list_add(&bnep_channels, (btstack_linked_item_t *) channel);
    
    return channel;
}

static bnep_channel_t* bnep_channel_for_addr(bd_addr_t addr)
{
    btstack_linked_item_t *it;
    for (it = (btstack_linked_item_t *) bnep_channels; it ; it = it->next){
        bnep_channel_t *channel = ((bnep_channel_t *) it);
        if (bd_addr_cmp(addr, channel->remote_addr) == 0) {
            return channel;
        }
    }
    return NULL;
}

static bnep_channel_t * bnep_channel_for_l2cap_cid(uint16_t l2cap_cid)
{
    btstack_linked_item_t *it;
    for (it = (btstack_linked_item_t *) bnep_channels; it ; it = it->next){
        bnep_channel_t *channel = ((bnep_channel_t *) it);
        if (channel->l2cap_cid == l2cap_cid) {
            return channel;
        }
    }
    return NULL;
}

static bnep_service_t * bnep_service_for_uuid(uint16_t uuid)
{
    btstack_linked_item_t *it;
    for (it = (btstack_linked_item_t *) bnep_services; it ; it = it->next){
        bnep_service_t * service = ((bnep_service_t *) it);
        if ( service->service_uuid == uuid){
            return service;
        }
    }
    return NULL;
}

static void bnep_channel_free(bnep_channel_t *channel)
{
    btstack_linked_list_remove( &bnep_channels, (btstack_linked_item_t *) channel);
    btstack_memory_bnep_channel_free(channel);
}

static void bnep_channel_finalize(bnep_channel_t *channel)
{    
    uint16_t l2cap_cid;
    
    /* Inform application about closed channel */
    if (channel->state == BNEP_CHANNEL_STATE_CONNECTED) {
        bnep_emit_channel_closed(channel);
    }

    l2cap_cid = channel->l2cap_cid;

    /* Stop any eventually running timer */
    bnep_channel_stop_timer(channel);
    
    /* Free ressources and then close the l2cap channel */
    bnep_channel_free(channel);
    l2cap_disconnect(l2cap_cid);
}

static int bnep_handle_connection_request(bnep_channel_t *channel, uint8_t *packet, uint16_t size)
{
    uint16_t uuid_size;
    uint16_t uuid_offset = 0; // avoid "may be unitialized when used" in clang
    uuid_size = packet[1];
    uint16_t response_code = BNEP_RESP_SETUP_SUCCESS;
    bnep_service_t * service;

    /* Sanity check packet size */
    if (size < (1 + 1 + (2 * uuid_size))) {
        return 0;
    }

    if ((channel->state != BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_REQUEST) &&
        (channel->state != BNEP_CHANNEL_STATE_CONNECTED)) {
        /* Ignore a connection request if not waiting for or still connected */
        log_error("BNEP_CONNECTION_REQUEST: ignored in state %d, l2cap_cid: %d!", channel->state, channel->l2cap_cid);
        return 0;
    }

     /* Extract source and destination UUID and convert them to UUID16 format */
    switch (uuid_size) {
        case 2:  /* UUID16  */
            uuid_offset = 0;
            break;
        case 4:  /* UUID32  */
        case 16: /* UUID128 */
            uuid_offset = 2;
            break;
        default:
            log_error("BNEP_CONNECTION_REQUEST: Invalid UUID size %d, l2cap_cid: %d!", channel->state, channel->l2cap_cid);
            response_code = BNEP_RESP_SETUP_INVALID_SERVICE_UUID_SIZE;
            break;
    }

    /* Check bits 16-31 of UUID */
    if (uuid_size > 2){
        uint16_t dest_prefix = big_endian_read_16(packet, 2);
        if (dest_prefix != 0){
            response_code = BNEP_RESP_SETUP_INVALID_DEST_UUID;
        }
        uint16_t src_prefix = big_endian_read_16(packet, 2 + uuid_size);
        if (src_prefix != 0){
            response_code = BNEP_RESP_SETUP_INVALID_SOURCE_UUID;
        }
    }

    /* check bits 32-127 of UUID */
    if (uuid_size == 16){
        if (uuid_has_bluetooth_prefix(&packet[2]) == false){
            response_code = BNEP_RESP_SETUP_INVALID_DEST_UUID;
        }
        if (uuid_has_bluetooth_prefix(&packet[2+16]) == false){
            response_code = BNEP_RESP_SETUP_INVALID_SOURCE_UUID;
        }
    }

    /* Check source and destination UUIDs for valid combinations */
    if (response_code == BNEP_RESP_SETUP_SUCCESS) {
        channel->uuid_dest = big_endian_read_16(packet, 2 + uuid_offset);
        channel->uuid_source = big_endian_read_16(packet, 2 + uuid_offset + uuid_size);

        if ((channel->uuid_dest != BLUETOOTH_SERVICE_CLASS_PANU) && 
            (channel->uuid_dest != BLUETOOTH_SERVICE_CLASS_NAP) &&
            (channel->uuid_dest != BLUETOOTH_SERVICE_CLASS_GN)) {
            log_error("BNEP_CONNECTION_REQUEST: Invalid destination service UUID: %04x", channel->uuid_dest);
            channel->uuid_dest = 0;
        }    
        if ((channel->uuid_source != BLUETOOTH_SERVICE_CLASS_PANU) && 
            (channel->uuid_source != BLUETOOTH_SERVICE_CLASS_NAP) &&
            (channel->uuid_source != BLUETOOTH_SERVICE_CLASS_GN)) {
            log_error("BNEP_CONNECTION_REQUEST: Invalid source service UUID: %04x", channel->uuid_source);
            channel->uuid_source = 0;
        }

        /* Check if we have registered a service for the requested destination UUID */
        service = bnep_service_for_uuid(channel->uuid_dest);
        if (service == NULL) {
            response_code = BNEP_RESP_SETUP_INVALID_DEST_UUID;
        } else {
            // use packet handler for service
            channel->packet_handler = service->packet_handler;

            if ((channel->uuid_source != BLUETOOTH_SERVICE_CLASS_PANU) && (channel->uuid_dest != BLUETOOTH_SERVICE_CLASS_PANU)) {
                response_code = BNEP_RESP_SETUP_INVALID_SOURCE_UUID;
            }
        } 
    }

    /* Set flag to send out the connection response on next statemachine cycle */
    bnep_channel_state_add(channel, BNEP_CHANNEL_STATE_VAR_SND_CONNECTION_RESPONSE);
    channel->response_code = response_code;
    l2cap_request_can_send_now_event(channel->l2cap_cid);
        
    /* Return the number of processed package bytes = BNEP Type, BNEP Control Type, UUID-Size + 2 * UUID */
    return 1 + 1 + (2 * uuid_size);
}

static int bnep_handle_connection_response(bnep_channel_t *channel, uint8_t *packet, uint16_t size)
{
    uint16_t response_code;

    /* Sanity check packet size */
    if (size < (1 + 2)) {
        return 0;
    }

    if (channel->state != BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_RESPONSE) {
        /* Ignore a connection response in any state but WAIT_FOR_CONNECTION_RESPONSE */
        log_error("BNEP_CONNECTION_RESPONSE: Ignored in channel state %d", channel->state);
        return 1 + 2;
    }

    response_code = big_endian_read_16(packet, 1);

    if (response_code == BNEP_RESP_SETUP_SUCCESS) {
        log_info("BNEP_CONNECTION_RESPONSE: Channel established to %s", bd_addr_to_str(channel->remote_addr));
        channel->state = BNEP_CHANNEL_STATE_CONNECTED;
        /* Stop timeout timer! */
        bnep_channel_stop_timer(channel);
        bnep_emit_open_channel_complete(channel, 0);
    } else {
        log_error("BNEP_CONNECTION_RESPONSE: Connection to %s failed. Err: %d", bd_addr_to_str(channel->remote_addr), response_code);
        bnep_channel_finalize(channel);
    }
    return 1 + 2;
}

static int bnep_can_handle_extensions(bnep_channel_t * channel){
    /* Extension are primarily handled in CONNECTED state */
    if (channel->state == BNEP_CHANNEL_STATE_CONNECTED) return 1;
    /* and if we've received connection request, but haven't sent the reponse yet. */
    if ((channel->state == BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_REQUEST) &&
        (channel->state_var & BNEP_CHANNEL_STATE_VAR_SND_CONNECTION_RESPONSE)) {
        return 1;
    }
    return 0;
}

static int bnep_handle_filter_net_type_set(bnep_channel_t *channel, uint8_t *packet, uint16_t size)
{
    uint16_t list_length;
    uint16_t response_code = BNEP_RESP_FILTER_SUCCESS;
    
    /* Sanity check packet size */
    if (size < 3) {
        return 0;
    }
    
    list_length = big_endian_read_16(packet, 1);
    /* Sanity check packet size again with known package size */
    if (size < (3 + list_length)) {
        return 0;
    }

    if (!bnep_can_handle_extensions(channel)){
        log_error("BNEP_FILTER_NET_TYPE_SET: Ignored in channel state %d", channel->state);
        return 3 + list_length;
    }

    /* Check if we have enough space for more filters */
    if ((list_length / (2*2)) > MAX_BNEP_NETFILTER) {
        log_info("BNEP_FILTER_NET_TYPE_SET: Too many filter");         
        response_code = BNEP_RESP_FILTER_ERR_TOO_MANY_FILTERS;
    } else {
        int i;
        channel->net_filter_count = 0;
        /* There is still enough space, copy the filters to our filter list */
        /* There is still enough space, copy the filters to our filter list */
        for (i = 0; i < (list_length / (2 * 2)); i ++) {
            channel->net_filter[channel->net_filter_count].range_start = big_endian_read_16(packet, 1 + 2 + (i * 4));
            channel->net_filter[channel->net_filter_count].range_end = big_endian_read_16(packet, 1 + 2 + (i * 4) + 2);
            if (channel->net_filter[channel->net_filter_count].range_start > channel->net_filter[channel->net_filter_count].range_end) {
                /* Invalid filter range, ignore this filter rule */
                log_error("BNEP_FILTER_NET_TYPE_SET: Invalid filter: start: %d, end: %d", 
                         channel->net_filter[channel->net_filter_count].range_start,
                         channel->net_filter[channel->net_filter_count].range_end);                
                response_code = BNEP_RESP_FILTER_ERR_INVALID_RANGE;
            } else {
                /* Valid filter, increase the filter count */
                log_info("BNEP_FILTER_NET_TYPE_SET: Add filter: start: %d, end: %d", 
                         channel->net_filter[channel->net_filter_count].range_start,
                         channel->net_filter[channel->net_filter_count].range_end);
                channel->net_filter_count ++;
            }
        }
    }

    /* Set flag to send out the set net filter response on next statemachine cycle */
    bnep_channel_state_add(channel, BNEP_CHANNEL_STATE_VAR_SND_FILTER_NET_TYPE_RESPONSE);
    channel->response_code = response_code;
    l2cap_request_can_send_now_event(channel->l2cap_cid);

    return 3 + list_length;
}

static int bnep_handle_filter_net_type_response(bnep_channel_t *channel, uint8_t *packet, uint16_t size)
{
	uint16_t response_code;

    // TODO: Currently we do not support setting a network filter.
    
    /* Sanity check packet size */
    if (size < (1 + 2)) {
        return 0;
    }

    if (!bnep_can_handle_extensions(channel)){
        log_error("BNEP_FILTER_NET_TYPE_RESPONSE: Ignored in channel state %d", channel->state);
        return 1 + 2;
    }

    response_code = big_endian_read_16(packet, 1);

    if (response_code == BNEP_RESP_FILTER_SUCCESS) {
        log_info("BNEP_FILTER_NET_TYPE_RESPONSE: Net filter set successfully for %s", bd_addr_to_str(channel->remote_addr));
    } else {
        log_error("BNEP_FILTER_NET_TYPE_RESPONSE: Net filter setting for %s failed. Err: %d", bd_addr_to_str(channel->remote_addr), response_code);
    }

    return 1 + 2;
}

static int bnep_handle_multi_addr_set(bnep_channel_t *channel, uint8_t *packet, uint16_t size)
{
    uint16_t list_length;
    uint16_t response_code = BNEP_RESP_FILTER_SUCCESS;

    /* Sanity check packet size */
    if (size < 3) {
        return 0;
    }
    
    list_length = big_endian_read_16(packet, 1);
    /* Sanity check packet size again with known package size */
    if (size < (3 + list_length)) {
        return 0;
    }

    if (!bnep_can_handle_extensions(channel)){
        log_error("BNEP_MULTI_ADDR_SET: Ignored in channel state %d", channel->state);
        return 3 + list_length;
    }

    /* Check if we have enough space for more filters */
	uint16_t list_count = list_length / (2 * ETHER_ADDR_LEN);
    if (list_count > MAX_BNEP_MULTICAST_FILTER) {
        log_info("BNEP_MULTI_ADDR_SET: Too many filter");         
        response_code = BNEP_RESP_FILTER_ERR_TOO_MANY_FILTERS;
    } else {
        unsigned int i;
        channel->multicast_filter_count = 0;
        /* There is enough space, copy the filters to our filter list */
        for (i = 0; i < list_count; i ++) {
            bd_addr_copy(channel->multicast_filter[channel->multicast_filter_count].addr_start, packet + 1 + 2 + (i * ETHER_ADDR_LEN * 2));
            bd_addr_copy(channel->multicast_filter[channel->multicast_filter_count].addr_end, packet + 1 + 2 + (i * ETHER_ADDR_LEN * 2) + ETHER_ADDR_LEN);

            if (memcmp(channel->multicast_filter[channel->multicast_filter_count].addr_start, 
                       channel->multicast_filter[channel->multicast_filter_count].addr_end, ETHER_ADDR_LEN) > 0) {
                /* Invalid filter range, ignore this filter rule */
                log_error("BNEP_MULTI_ADDR_SET: Invalid filter: start: %s", 
                         bd_addr_to_str(channel->multicast_filter[channel->multicast_filter_count].addr_start));
                log_error("BNEP_MULTI_ADDR_SET: Invalid filter: end: %s",
                         bd_addr_to_str(channel->multicast_filter[channel->multicast_filter_count].addr_end));
                response_code = BNEP_RESP_FILTER_ERR_INVALID_RANGE;
            } else {
                /* Valid filter, increase the filter count */
                log_info("BNEP_MULTI_ADDR_SET: Add filter: start: %s", 
                         bd_addr_to_str(channel->multicast_filter[channel->multicast_filter_count].addr_start));
                log_info("BNEP_MULTI_ADDR_SET: Add filter: end: %s",
                         bd_addr_to_str(channel->multicast_filter[channel->multicast_filter_count].addr_end));
                channel->multicast_filter_count ++;
            }
        }
    }
    /* Set flag to send out the set multi addr response on next statemachine cycle */
    bnep_channel_state_add(channel, BNEP_CHANNEL_STATE_VAR_SND_FILTER_MULTI_ADDR_RESPONSE);
    channel->response_code = response_code;
    l2cap_request_can_send_now_event(channel->l2cap_cid);
    
    return 3 + list_length;
}

static int bnep_handle_multi_addr_response(bnep_channel_t *channel, uint8_t *packet, uint16_t size)
{
	uint16_t response_code;

    // TODO: Currently we do not support setting multicast address filter.
    
    /* Sanity check packet size */
    if (size < (1 + 2)) {
        return 0;
    }

    if (!bnep_can_handle_extensions(channel)){
        log_error("BNEP_MULTI_ADDR_RESPONSE: Ignored in channel state %d", channel->state);
        return 1 + 2;
    }

    response_code = big_endian_read_16(packet, 1);

    if (response_code == BNEP_RESP_FILTER_SUCCESS) {
        log_info("BNEP_MULTI_ADDR_RESPONSE: Multicast address filter set successfully for %s", bd_addr_to_str(channel->remote_addr));
    } else {
        log_error("BNEP_MULTI_ADDR_RESPONSE: Multicast address filter setting for %s failed. Err: %d", bd_addr_to_str(channel->remote_addr), response_code);
    }

    return 1 + 2;
}

static int bnep_handle_ethernet_packet(bnep_channel_t *channel, bd_addr_t addr_dest, bd_addr_t addr_source, uint16_t network_protocol_type, uint8_t *payload, uint16_t size)
{
    uint16_t pos = 0;
    
#if defined(HCI_INCOMING_PRE_BUFFER_SIZE) && (HCI_INCOMING_PRE_BUFFER_SIZE >= 14 - 8) // 2 * sizeof(bd_addr_t) + sizeof(uint16_t) - L2CAP Header (4) - ACL Header (4)
    /* In-place modify the package and add the ethernet header in front of the payload.
     * WARNING: This modifies the data in front of the payload and may overwrite 14 bytes there!
     */
    uint8_t *ethernet_packet = payload - (2 * sizeof(bd_addr_t)) - sizeof(uint16_t);
    /* Restore the ethernet packet header */
    bd_addr_copy(ethernet_packet + pos, addr_dest);
    pos += sizeof(bd_addr_t);
    bd_addr_copy(ethernet_packet + pos, addr_source);
    pos += sizeof(bd_addr_t);
    big_endian_store_16(ethernet_packet, pos, network_protocol_type);
    /* Payload is just in place... */
#else
#error "BNEP requires HCI_INCOMING_PRE_BUFFER_SIZE >= 6. Please update bstack_config.h"
#endif
    
    /* Notify application layer and deliver the ethernet packet */
    if (channel->packet_handler){
        (*channel->packet_handler)(BNEP_DATA_PACKET, channel->l2cap_cid, ethernet_packet,
                                   size + sizeof(uint16_t) + (2 * sizeof(bd_addr_t)) );
    }
    
    return size;
}

static int bnep_handle_control_packet(bnep_channel_t *channel, uint8_t *packet, uint16_t size, int is_extension)
{
    uint16_t len = 0;

    if (size > 0) {

        uint8_t bnep_control_type = packet[0];
        /* Save last control type. Needed by statemachin in case of unknown control code */

        channel->last_control_type = bnep_control_type;
        log_info("BNEP_CONTROL: Type: %d, size: %d, is_extension: %d", bnep_control_type, size, is_extension);
        switch (bnep_control_type) {
            case BNEP_CONTROL_TYPE_COMMAND_NOT_UNDERSTOOD:
                /* The last command we send was not understood. We should close the connection */
                log_error("BNEP_CONTROL: Received COMMAND_NOT_UNDERSTOOD: l2cap_cid: %d, cmd: %d", channel->l2cap_cid,
                          packet[3]);
                bnep_channel_finalize(channel);
                len = 2; // Length of command not understood packet - bnep-type field
                break;
            case BNEP_CONTROL_TYPE_SETUP_CONNECTION_REQUEST:
                if (is_extension) {
                    /* Connection requests are not allowed to be send in an extension header
                     *  ignore, do not set "COMMAND_NOT_UNDERSTOOD"
                     */
                    log_error("BNEP_CONTROL: Received SETUP_CONNECTION_REQUEST in extension header: l2cap_cid: %d",
                              channel->l2cap_cid);
                    return 0;
                } else {
                    len = bnep_handle_connection_request(channel, packet, size);
                }
                break;
            case BNEP_CONTROL_TYPE_SETUP_CONNECTION_RESPONSE:
                if (is_extension) {
                    /* Connection requests are not allowed to be send in an
                     * extension header, ignore, do not set "COMMAND_NOT_UNDERSTOOD"
                     */
                    log_error("BNEP_CONTROL: Received SETUP_CONNECTION_RESPONSE in extension header: l2cap_cid: %d",
                              channel->l2cap_cid);
                    return 0;
                } else {
                    len = bnep_handle_connection_response(channel, packet, size);
                }
                break;
            case BNEP_CONTROL_TYPE_FILTER_NET_TYPE_SET:
                len = bnep_handle_filter_net_type_set(channel, packet, size);
                break;
            case BNEP_CONTROL_TYPE_FILTER_NET_TYPE_RESPONSE:
                len = bnep_handle_filter_net_type_response(channel, packet, size);
                break;
            case BNEP_CONTROL_TYPE_FILTER_MULTI_ADDR_SET:
                len = bnep_handle_multi_addr_set(channel, packet, size);
                break;
            case BNEP_CONTROL_TYPE_FILTER_MULTI_ADDR_RESPONSE:
                len = bnep_handle_multi_addr_response(channel, packet, size);
                break;
            default:
                log_error("BNEP_CONTROL: Invalid bnep control type: l2cap_cid: %d, cmd: %d", channel->l2cap_cid,
                          bnep_control_type);
                len = 0;
                break;
        }
    }

    if (len == 0) {
        /* In case the command could not be handled, send a 
           COMMAND_NOT_UNDERSTOOD message. 
           Set flag to process the request in the next statemachine loop 
         */
        bnep_channel_state_add(channel, BNEP_CHANNEL_STATE_VAR_SND_NOT_UNDERSTOOD);        
        l2cap_request_can_send_now_event(channel->l2cap_cid);
    }

    return len;
}

/**
 * @return handled packet
 */
static int bnep_hci_event_handler(uint8_t *packet, uint16_t size)
{
    UNUSED(size);   // ok: handling own l2cap events

    bd_addr_t event_addr;
    uint16_t  psm;
    uint16_t  l2cap_cid;
    hci_con_handle_t con_handle;
    bnep_channel_t  *channel = NULL;
    uint8_t   status;
    
    switch (hci_event_packet_get_type(packet)) {
            
        /* Accept an incoming L2CAP connection on BLUETOOTH_PSM_BNEP */
        case L2CAP_EVENT_INCOMING_CONNECTION:
            /* L2CAP event data: event(8), len(8), address(48), handle (16),  psm (16), source cid(16) dest cid(16) */
            reverse_bd_addr(&packet[2], event_addr);
            con_handle = little_endian_read_16(packet,  8);
            psm        = little_endian_read_16(packet, 10); 
            l2cap_cid  = little_endian_read_16(packet, 12); 

            if (psm != BLUETOOTH_PSM_BNEP) break;

            channel = bnep_channel_for_addr(event_addr);

            if (channel) {                
                log_error("INCOMING_CONNECTION (l2cap_cid 0x%02x) for BLUETOOTH_PSM_BNEP => decline - channel already exists", l2cap_cid);
                l2cap_decline_connection(l2cap_cid);
                return 1;
            }
            
            /* Create a new BNEP channel instance (incoming) */
            channel = bnep_channel_create_for_addr(event_addr);

            if (!channel) {
                log_error("INCOMING_CONNECTION (l2cap_cid 0x%02x) for BLUETOOTH_PSM_BNEP => decline - no memory left", l2cap_cid);
                l2cap_decline_connection(l2cap_cid);
                return 1;
            }

            /* Assign connection handle and l2cap cid */
            channel->con_handle = con_handle;
            channel->l2cap_cid = l2cap_cid;

            /* Set channel into accept state */
            channel->state = BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_REQUEST;

            /* Start connection timeout timer */
            bnep_channel_start_timer(channel, BNEP_CONNECTION_TIMEOUT_MS);
            
            log_info("L2CAP_EVENT_INCOMING_CONNECTION (l2cap_cid 0x%02x) for BLUETOOTH_PSM_BNEP => accept", l2cap_cid);
            l2cap_accept_connection(l2cap_cid);
            return 1;
            
        /* Outgoing L2CAP connection has been opened -> store l2cap_cid, remote_addr */
        case L2CAP_EVENT_CHANNEL_OPENED: 
            status = packet[2];
            log_info("L2CAP_EVENT_CHANNEL_OPENED for BLUETOOTH_PSM_BNEP, status %u", status);
            
            /* Get the bnep channel fpr remote address */
            con_handle = little_endian_read_16(packet, 9);
            l2cap_cid  = little_endian_read_16(packet, 13);
            reverse_bd_addr(&packet[3], event_addr);
            channel = bnep_channel_for_addr(event_addr);
            if (!channel) {
                log_error("L2CAP_EVENT_CHANNEL_OPENED but no BNEP channel prepared");
                return 1;
            }

            /* On L2CAP open error discard everything */
            if (status) {
                /* Emit bnep_open_channel_complete with status and free channel */
                bnep_emit_open_channel_complete(channel, status);

                /* Free BNEP channel mempory */
                bnep_channel_free(channel);
                return 1;
            }

            switch (channel->state){
                case BNEP_CHANNEL_STATE_CLOSED:
                    log_info("L2CAP_EVENT_CHANNEL_OPENED: outgoing connection");

                    bnep_channel_start_timer(channel, BNEP_CONNECTION_TIMEOUT_MS);

                    /* Assign connection handle and l2cap cid */
                    channel->l2cap_cid  = l2cap_cid;
                    channel->con_handle = con_handle;

                    /* Initiate the connection request */
                    channel->state = BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_RESPONSE;
                    bnep_channel_state_add(channel, BNEP_CHANNEL_STATE_VAR_SND_CONNECTION_REQUEST); 
                    channel->max_frame_size = bnep_max_frame_size_for_l2cap_mtu(little_endian_read_16(packet, 17));
                    l2cap_request_can_send_now_event(channel->l2cap_cid);
                    break;
                case BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_REQUEST:
                    /* New information: channel mtu */
                    channel->max_frame_size = bnep_max_frame_size_for_l2cap_mtu(little_endian_read_16(packet, 17));
                    break;
                default:
                    log_error("L2CAP_EVENT_CHANNEL_OPENED: Invalid state: %d", channel->state);
                    break;
            }
            return 1;
                    
        case L2CAP_EVENT_CAN_SEND_NOW:
            bnep_handle_can_send_now(l2cap_event_can_send_now_get_local_cid(packet));
            break;
            
        case L2CAP_EVENT_CHANNEL_CLOSED:
            // data: event (8), len(8), channel (16)
            l2cap_cid   = little_endian_read_16(packet, 2);
            channel = bnep_channel_for_l2cap_cid(l2cap_cid);
            log_info("L2CAP_EVENT_CHANNEL_CLOSED cid 0x%0x, channel %p", l2cap_cid, channel);

            if (!channel) {
                break;
            }

            log_info("L2CAP_EVENT_CHANNEL_CLOSED state %u", channel->state);
            switch (channel->state) {
                case BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_REQUEST:
                case BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_RESPONSE:
                case BNEP_CHANNEL_STATE_CONNECTED:
                    bnep_channel_finalize(channel);
                    return 1;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return 0;
}

static int bnep_l2cap_packet_handler(uint16_t l2cap_cid, uint8_t *packet, uint16_t size)
{
    int             rc = 0;
    uint8_t         bnep_type;
    uint8_t         bnep_header_has_ext;
    uint8_t         extension_type;
    uint16_t        pos = 0;
    bd_addr_t       addr_source;
    bd_addr_t       addr_dest;
    uint16_t        network_protocol_type = 0xffff;
    bnep_channel_t *channel = NULL;
    
    /* Get the bnep channel for this package */
    channel = bnep_channel_for_l2cap_cid(l2cap_cid);
    if (!channel) {
        return rc;
    }

    /* Sort out short packages */
    if (size < 2) {
        return rc;
    }
    
    bnep_type = BNEP_TYPE(packet[pos]);
    bnep_header_has_ext = BNEP_HEADER_HAS_EXT(packet[pos]); 
    pos ++;

    switch(bnep_type) {
        case BNEP_PKT_TYPE_GENERAL_ETHERNET:
            if ((pos + 14) > size) {
                return rc;
            }
            bd_addr_copy(addr_dest, &packet[pos]);
            pos += sizeof(bd_addr_t);
            bd_addr_copy(addr_source, &packet[pos]);
            pos += sizeof(bd_addr_t);
            network_protocol_type = big_endian_read_16(packet, pos);
            pos += 2;
            break;
        case BNEP_PKT_TYPE_COMPRESSED_ETHERNET:
            if ((pos + 2) > size) {
                return rc;
            }
            bd_addr_copy(addr_dest, channel->local_addr);
            bd_addr_copy(addr_source, channel->remote_addr);
            network_protocol_type = big_endian_read_16(packet, pos);
            pos += 2;
            break;
        case BNEP_PKT_TYPE_COMPRESSED_ETHERNET_SOURCE_ONLY:
            if ((pos + 8) > size) {
                return rc;
            }
            bd_addr_copy(addr_dest, channel->local_addr);
            bd_addr_copy(addr_source, &packet[pos]);
            pos += sizeof(bd_addr_t);
            network_protocol_type = big_endian_read_16(packet, pos);
            pos += 2;
            break;
        case BNEP_PKT_TYPE_COMPRESSED_ETHERNET_DEST_ONLY:
            if ((pos + 8) > size) {
                return rc;
            }
            bd_addr_copy(addr_dest, &packet[pos]);
            pos += sizeof(bd_addr_t);
            bd_addr_copy(addr_source, channel->remote_addr);
            network_protocol_type = big_endian_read_16(packet, pos);
            pos += 2;
            break;
        case BNEP_PKT_TYPE_CONTROL:
            rc = bnep_handle_control_packet(channel, packet + pos, size - pos, 0);
            if (rc == 0){
                // invalid control packet
                return 0;
            }
            pos += rc;
            break;
        default:
            break;
    }

    if (bnep_header_has_ext) {
        do {
            uint8_t ext_len;

            if (pos + 2 > size) {
                return rc;
            }

            /* Read extension type and check for further extensions */
            extension_type        = BNEP_TYPE(packet[pos]);
            bnep_header_has_ext   = BNEP_HEADER_HAS_EXT(packet[pos]);
            pos ++;

            /* Read extension header length */
            ext_len = packet[pos];
            pos ++;

            if ((size - pos) < ext_len) {
                return 0;
            }

            switch (extension_type) {
                case BNEP_EXT_HEADER_TYPE_EXTENSION_CONTROL:
                    if (ext_len != bnep_handle_control_packet(channel, packet + pos, ext_len, 1)) {
                        log_error("BNEP pkt handler: Ignore invalid control packet in extension header");
                    }

                    pos += ext_len;
                    break;
                    
                default:
                    /* Extension header type unknown. Unknown extension SHALL be forwarded
                     * in any way. But who shall handle these extension packets?
                     * For now: We ignore them and just drop them! 
                     */
                    log_error("BNEP pkt handler: Unknown extension type ignored, data dropped!");
                    pos += ext_len;
                    break;
            }

        } while (bnep_header_has_ext);
    }

    if ((bnep_type != BNEP_PKT_TYPE_CONTROL) && (network_protocol_type != 0xffff)) {
        if (channel->state == BNEP_CHANNEL_STATE_CONNECTED) {
            rc = bnep_handle_ethernet_packet(channel, addr_dest, addr_source, network_protocol_type, packet + pos, size - pos);
        } else {
            rc = 0;
        }
    }
    
    return rc;

}

void bnep_packet_handler(uint8_t packet_type, uint16_t l2cap_cid, uint8_t *packet, uint16_t size)
{
    switch (packet_type) {
        case HCI_EVENT_PACKET:
            bnep_hci_event_handler(packet, size);
            break;
        case L2CAP_DATA_PACKET:
            bnep_l2cap_packet_handler(l2cap_cid, packet, size);
            break;
        default:
            break;
    }
}

static void bnep_channel_state_machine(bnep_channel_t* channel, bnep_channel_event_t *event)
{
    log_debug("bnep_state_machine: state %u, state var: %02x, event %u", channel->state, channel->state_var, event->type);
 
    if (event->type == BNEP_CH_EVT_READY_TO_SEND) {
        /* Send outstanding packets. */
        if (channel->state_var & BNEP_CHANNEL_STATE_VAR_SND_NOT_UNDERSTOOD) {
            bnep_channel_state_remove(channel, BNEP_CHANNEL_STATE_VAR_SND_NOT_UNDERSTOOD);
            bnep_send_command_not_understood(channel, channel->last_control_type);
            return;
        }        
        if (channel->state_var & BNEP_CHANNEL_STATE_VAR_SND_CONNECTION_REQUEST) {
            bnep_channel_state_remove(channel, BNEP_CHANNEL_STATE_VAR_SND_CONNECTION_REQUEST);
            channel->state = BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_RESPONSE;
            bnep_send_connection_request(channel, channel->uuid_source, channel->uuid_dest);
        }
        if (channel->state_var & BNEP_CHANNEL_STATE_VAR_SND_CONNECTION_RESPONSE) {
            int emit_connected = 0;
            if ((channel->state == BNEP_CHANNEL_STATE_CLOSED) ||
                (channel->state == BNEP_CHANNEL_STATE_WAIT_FOR_CONNECTION_REQUEST)) {
                /* Set channel state to STATE_CONNECTED */
                channel->state = BNEP_CHANNEL_STATE_CONNECTED;
                /* Stop timeout timer! */
                bnep_channel_stop_timer(channel);
                emit_connected = 1;
            }
            
            bnep_channel_state_remove(channel, BNEP_CHANNEL_STATE_VAR_SND_CONNECTION_RESPONSE);
            bnep_send_connection_response(channel, channel->response_code);
            if (emit_connected){
                bnep_emit_open_channel_complete(channel, 0);
            }
            return;
        }
        if (channel->state_var & BNEP_CHANNEL_STATE_VAR_SND_FILTER_NET_TYPE_SET) {
            bnep_channel_state_remove(channel, BNEP_CHANNEL_STATE_VAR_SND_FILTER_NET_TYPE_SET);
            if ((channel->net_filter_out_count > 0) && (channel->net_filter_out != NULL)) {
                bnep_send_filter_net_type_set(channel, channel->net_filter_out, channel->net_filter_out_count);
                channel->net_filter_out_count = 0;
                channel->net_filter_out = NULL;
            }
            return;
        }
        if (channel->state_var & BNEP_CHANNEL_STATE_VAR_SND_FILTER_NET_TYPE_RESPONSE) {
            bnep_channel_state_remove(channel, BNEP_CHANNEL_STATE_VAR_SND_FILTER_NET_TYPE_RESPONSE);
            bnep_send_filter_net_type_response(channel, channel->response_code);
            return;
        }
        if (channel->state_var & BNEP_CHANNEL_STATE_VAR_SND_FILTER_MULTI_ADDR_SET) {
            bnep_channel_state_remove(channel, BNEP_CHANNEL_STATE_VAR_SND_FILTER_MULTI_ADDR_SET);
            if ((channel->multicast_filter_out_count > 0) && (channel->multicast_filter_out != NULL)) {
                bnep_send_filter_multi_addr_set(channel, channel->multicast_filter_out, channel->multicast_filter_out_count);
                channel->multicast_filter_out_count = 0;
                channel->multicast_filter_out = NULL;
            }
            return;
        }
        if (channel->state_var & BNEP_CHANNEL_STATE_VAR_SND_FILTER_MULTI_ADDR_RESPONSE) {
            bnep_channel_state_remove(channel, BNEP_CHANNEL_STATE_VAR_SND_FILTER_MULTI_ADDR_RESPONSE);
            bnep_send_filter_multi_addr_response(channel, channel->response_code);
            return;
        }

        /* If the event was not yet handled, notify the application layer */
        if (channel->waiting_for_can_send_now){
            channel->waiting_for_can_send_now = 0;            
            bnep_emit_ready_to_send(channel);
        }
    }    
}

static void bnep_handle_can_send_now(uint16_t l2cap_cid){
    btstack_linked_item_t *it;
    btstack_linked_item_t *next;
    
    for (it = (btstack_linked_item_t *) bnep_channels; it ; it = next){
        next = it->next;    // be prepared for removal of channel in state machine
        bnep_channel_t * channel = ((bnep_channel_t *) it);
        if (channel->l2cap_cid != l2cap_cid) continue;
        //
        bnep_channel_event_t channel_event = { BNEP_CH_EVT_READY_TO_SEND };
        bnep_channel_state_machine(channel, &channel_event);

        if (!l2cap_can_send_packet_now(channel->l2cap_cid)) {
            l2cap_request_can_send_now_event(channel->l2cap_cid);
            return;
        }
    }
}


/* BNEP BTStack API */
void bnep_init(void)
{
    bnep_security_level = gap_get_security_level();
}

void bnep_deinit(void){
    bnep_services = NULL;
    bnep_channels = NULL;
    bnep_security_level = 0;
}

void bnep_set_required_security_level(gap_security_level_t security_level)
{
    bnep_security_level = security_level;
}

int bnep_connect(btstack_packet_handler_t packet_handler, bd_addr_t addr, uint16_t l2cap_psm, uint16_t uuid_src, uint16_t uuid_dest)
{
    bnep_channel_t *channel;
    log_info("BNEP_CONNECT addr %s", bd_addr_to_str(addr));

    channel = bnep_channel_create_for_addr(addr);
    if (channel == NULL) {
        return -1;
    }

    channel->uuid_source    = uuid_src;
    channel->uuid_dest      = uuid_dest;
    channel->packet_handler = packet_handler;

    uint8_t status = l2cap_create_channel(bnep_packet_handler, addr, l2cap_psm, l2cap_max_mtu(), NULL);
    if (status){
        return -1;
    }
    return 0;
}

void bnep_disconnect(bd_addr_t addr)
{
    bnep_channel_t *channel;
    log_info("BNEP_DISCONNECT");

    channel = bnep_channel_for_addr(addr);
    
    bnep_channel_finalize(channel);
}


uint8_t bnep_register_service(btstack_packet_handler_t packet_handler, uint16_t service_uuid, uint16_t max_frame_size)
{
    log_info("BNEP_REGISTER_SERVICE mtu %d", max_frame_size);

    /* Check if we already registered a service */
    bnep_service_t * service = bnep_service_for_uuid(service_uuid);
    if (service) {
        return BNEP_SERVICE_ALREADY_REGISTERED;
    }

    /* Only alow one the three service types: PANU, NAP, GN */
    if ((service_uuid != BLUETOOTH_SERVICE_CLASS_PANU) && 
        (service_uuid != BLUETOOTH_SERVICE_CLASS_NAP) &&
        (service_uuid != BLUETOOTH_SERVICE_CLASS_GN)) {
        log_info("BNEP_REGISTER_SERVICE: Invalid service UUID: %04x", service_uuid);
        return BNEP_SERVICE_ALREADY_REGISTERED; // TODO: define own error
    }
    
    /* Allocate service memory */
    service = (bnep_service_t*) btstack_memory_bnep_service_get();
    if (!service) {
        return BTSTACK_MEMORY_ALLOC_FAILED;
    }

    /* register with l2cap if not registered before, max MTU */
    l2cap_register_service(bnep_packet_handler, BLUETOOTH_PSM_BNEP, 0xffff, bnep_security_level);
        
    /* Setup the service struct */
    service->max_frame_size = max_frame_size;
    service->service_uuid    = service_uuid;
    service->packet_handler = packet_handler;


    /* Add to services list */
    btstack_linked_list_add(&bnep_services, (btstack_linked_item_t *) service);
    
    return 0;
}

void bnep_unregister_service(uint16_t service_uuid)
{
    log_info("BNEP_UNREGISTER_SERVICE #%04x", service_uuid);

    bnep_service_t *service = bnep_service_for_uuid(service_uuid);    
    if (!service) {
        return;
    }

    btstack_linked_list_remove(&bnep_services, (btstack_linked_item_t *) service);
    btstack_memory_bnep_service_free(service);
    service = NULL;
    
    l2cap_unregister_service(BLUETOOTH_PSM_BNEP);
}

