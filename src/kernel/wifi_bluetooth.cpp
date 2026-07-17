#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern "C" {
    // Current statuses
    bool wifi_connected = false;
    char connected_ssid[32] = "Not Connected";
    int wifi_signal_strength = 0; // 0..4 bars
    
    bool bluetooth_enabled = true;
    char paired_device_name[32] = "No Device Paired";
    bool bluetooth_connected = false;

    // Simulated Wi-Fi Networks Database
    typedef struct {
        const char* ssid;
        int signal; // 1..4
        bool secure;
    } WifiNetwork;

    static WifiNetwork mock_wifi_list[] = {
        {"BlankOS_Secure", 4, true},
        {"Ethan_WiFi_5G", 3, true},
        {"Airport_Free_WiFi", 2, false},
        {"Coffee_Shop", 3, true},
        {"Home_Network", 4, true}
    };
    int mock_wifi_count = 5;

    // Simulated Bluetooth Devices Database
    typedef struct {
        const char* name;
        const char* type; // "audio", "input", "other"
        bool paired;
    } BTDevice;

    static BTDevice mock_bt_list[] = {
        {"AirPods Max", "audio", false},
        {"Magic Keyboard", "input", false},
        {"MX Master 3S", "input", false},
        {"Bose QC35", "audio", false},
        {"iPhone 15 Pro", "other", false}
    };
    int mock_bt_count = 5;

    // Methods
    int get_wifi_networks_count() { return mock_wifi_count; }
    const char* get_wifi_ssid(int idx) { return mock_wifi_list[idx].ssid; }
    int get_wifi_signal(int idx) { return mock_wifi_list[idx].signal; }
    bool is_wifi_secure(int idx) { return mock_wifi_list[idx].secure; }

    int get_bt_devices_count() { return mock_bt_count; }
    const char* get_bt_name(int idx) { return mock_bt_list[idx].name; }
    const char* get_bt_type(int idx) { return mock_bt_list[idx].type; }
    bool is_bt_paired(int idx) { return mock_bt_list[idx].paired; }

    bool connect_to_wifi(const char* ssid, const char* password) {
        // Basic simulation: if password is entered or open network
        for (int i = 0; i < mock_wifi_count; i++) {
            if (mock_wifi_list[i].ssid == ssid) {
                // If secure, check password (accept anything longer than 4 chars)
                int len = 0;
                while (password && password[len]) len++;
                
                if (!mock_wifi_list[i].secure || len >= 4) {
                    wifi_connected = true;
                    // Copy SSID
                    int j = 0;
                    while (ssid[j] && j < 31) {
                        connected_ssid[j] = ssid[j];
                        j++;
                    }
                    connected_ssid[j] = '\0';
                    wifi_signal_strength = mock_wifi_list[i].signal;
                    return true;
                }
            }
        }
        return false;
    }

    bool pair_bluetooth_device(const char* name) {
        for (int i = 0; i < mock_bt_count; i++) {
            if (mock_bt_list[i].name == name) {
                mock_bt_list[i].paired = true;
                bluetooth_connected = true;
                // Copy paired name
                int j = 0;
                while (name[j] && j < 31) {
                    paired_device_name[j] = name[j];
                    j++;
                }
                paired_device_name[j] = '\0';
                return true;
            }
        }
        return false;
    }

    void disconnect_wifi() {
        wifi_connected = false;
        connected_ssid[0] = '\0';
        wifi_signal_strength = 0;
    }

    void disconnect_bluetooth() {
        bluetooth_connected = false;
        paired_device_name[0] = '\0';
    }
}
