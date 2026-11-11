#include <efi.h>
#include <efilib.h>
#include <stdint.h>
#include <stdbool.h>
#include "net.h"

extern "C" {

static bool wifi_connected = false;
static char connected_ssid[32] = "Ethernet (UEFI SNP)";
static int wifi_signal_strength = 0;
static bool bluetooth_connected = false;
static char paired_device_name[32] = "No BT Controller";

void init_wifi_bluetooth(EFI_SYSTEM_TABLE* SystemTable) {
    // Delegate real NIC discovery + DHCP to net.cpp
    net_init(SystemTable);

    NetStatus st;
    net_get_status(&st);
    if (st.nic_present) {
        wifi_connected = st.link_up;
        wifi_signal_strength = st.link_up ? 4 : 0;
    } else {
        wifi_connected = false;
        wifi_signal_strength = 0;
    }
}

void net_init_hardware(EFI_SYSTEM_TABLE* SystemTable) {
    init_wifi_bluetooth(SystemTable);
}

// Compatibility getters backed by REAL network stack status in net.cpp
bool is_wifi_connected(void) {
    NetStatus st;
    net_get_status(&st);
    return st.nic_present && st.link_up;
}

const char* get_connected_ssid(void) {
    NetStatus st;
    net_get_status(&st);
    if (!st.nic_present) return "No Network Adapter";
    return st.adapter_name;
}

int get_wifi_signal_strength(void) {
    NetStatus st;
    net_get_status(&st);
    return (st.nic_present && st.link_up) ? 4 : 0;
}

bool is_bluetooth_connected(void) { return bluetooth_connected; }
const char* get_paired_bt_name(void) { return paired_device_name; }

const char* net_get_adapter_name(void) {
    static char buf[64];
    NetStatus st;
    net_get_status(&st);
    int i = 0;
    while (i < 63 && st.adapter_name[i]) { buf[i] = st.adapter_name[i]; i++; }
    buf[i] = 0;
    return buf;
}

const char* net_get_mac_str(void) {
    static char buf[24];
    NetStatus st;
    net_get_status(&st);
    int i = 0;
    while (i < 23 && st.mac_str[i]) { buf[i] = st.mac_str[i]; i++; }
    buf[i] = 0;
    return buf;
}

const char* net_get_ipv4_str(void) {
    static char buf[20];
    NetStatus st;
    net_get_status(&st);
    int i = 0;
    while (i < 19 && st.ip_str[i]) { buf[i] = st.ip_str[i]; i++; }
    buf[i] = 0;
    return buf;
}

uint32_t net_get_tx_packets(void) {
    NetStatus st;
    net_get_status(&st);
    return (uint32_t)st.tx_packets;
}

uint32_t net_get_rx_packets(void) {
    NetStatus st;
    net_get_status(&st);
    return (uint32_t)st.rx_packets;
}

// Genuine empty hardware scan results when no 802.11 / HCI firmware driver is attached
int get_wifi_networks_count(void) { return 0; }
const char* get_wifi_ssid(int idx) { (void)idx; return ""; }
int get_wifi_signal(int idx) { (void)idx; return 0; }
bool is_wifi_secure(int idx) { (void)idx; return false; }

int get_bt_devices_count(void) { return 0; }
const char* get_bt_name(int idx) { (void)idx; return ""; }
const char* get_bt_type(int idx) { (void)idx; return ""; }
bool is_bt_paired(int idx) { (void)idx; return false; }

bool connect_to_wifi(const char* ssid, const char* password) {
    (void)ssid; (void)password;
    return net_dhcp_renew();
}

bool pair_bluetooth_device(const char* name) {
    (void)name;
    return false;
}

void disconnect_wifi(void) {
    wifi_connected = false;
}

void disconnect_bluetooth(void) {
    bluetooth_connected = false;
}

} // extern "C"
