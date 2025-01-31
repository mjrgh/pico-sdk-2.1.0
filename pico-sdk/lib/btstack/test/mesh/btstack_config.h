//
// btstack_config.h for most tests
//

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// Port related features
#define HAVE_BTSTACK_STDIN
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

// BTstack features that can be enabled
#define ENABLE_BLE
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_MICRO_ECC_P256
#define ENABLE_PRINTF_HEXDUMP

// Mesh Config
#define ENABLE_MESH_ADV_BEARER
#define ENABLE_MESH_GATT_BEARER
#define ENABLE_MESH_PB_ADV
#define ENABLE_MESH_PB_GATT
#define ENABLE_MESH_PROXY_SERVER
#define ENABLE_MESH_RELAY

#define ENABLE_MESH
#define ENABLE_MESH_PROVISIONER

// BTstack configuration. buffers, sizes, ...
#define HCI_ACL_PAYLOAD_SIZE 1000
#define HCI_INCOMING_PRE_BUFFER_SIZE 4

#define MAX_NR_LE_DEVICE_DB_ENTRIES    4
#define MAX_NR_MESH_SUBNETS            2
#define MAX_NR_MESH_TRANSPORT_KEYS    16
#define MAX_NR_MESH_VIRTUAL_ADDRESSES 16

// allow for one NetKey update
#define MAX_NR_MESH_NETWORK_KEYS      (MAX_NR_MESH_SUBNETS+1)

#define NVM_NUM_LINK_KEYS 2

#endif
