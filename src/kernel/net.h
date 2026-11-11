#include <stddef.h>
#ifndef BLANKOS_NET_H
#define BLANKOS_NET_H

#include <efi.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Network Stack Status
typedef struct {
    bool nic_present;
    bool link_up;
    bool dhcp_bound;
    uint8_t mac[6];
    uint32_t ip_addr;        // Host byte order? No, network/dotted helpers use u8[4] packed big-endian (a<<24|b<<16|c<<8|d)
    uint32_t subnet_mask;
    uint32_t gateway_ip;
    uint32_t dns_ip;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    char adapter_name[64];
    char mac_str[24];
    char ip_str[20];
    char gw_str[20];
    char dns_str[20];
} NetStatus;

// HTTP Transfer States
typedef enum {
    HTTP_IDLE = 0,
    HTTP_RESOLVING_DNS = 1,
    HTTP_CONNECTING = 2,
    HTTP_SENDING = 3,
    HTTP_RECEIVING = 4,
    HTTP_DONE = 5,
    HTTP_ERROR = 6
} HttpState;

typedef struct {
    int id;
    HttpState state;
    int status_code;
    char url[512];
    char host[128];
    char path[384];
    uint16_t port;
    bool is_https;
    bool save_to_disk;
    char save_filename[128];
    char content_type[64];
    int content_length;      // -1 if chunked/unknown
    int bytes_received;
    int header_len;
    char* body_buf;          // Dynamically allocated buffer (or tail buffer when streaming to disk)
    int body_capacity;
    int speed_bps;
    char error_msg[128];
    bool active;
} HttpTransfer;

typedef struct {
    char filename[128];
    char url[512];
    char disk_path[192];
    int total_bytes;
    int downloaded_bytes;
    int speed_bps;
    HttpState state;
    bool saved_ok;
    char status_text[64];
} DownloadEntry;

// Initialize SNP driver, Ethernet/ARP/IPv4/UDP/TCP/DHCP/DNS stack
void net_init(EFI_SYSTEM_TABLE* st);

// Non-blocking network poll: receives Ethernet frames, advances TCP/HTTP/DHCP state machines
void net_tick(void);

// Trigger/renew DHCP lease (runs quick bounded poll or asynchronous state)
bool net_dhcp_renew(void);

// Retrieve current network status snapshot
void net_get_status(NetStatus* out);

// Send ICMP Echo Request (ping) to IPv4 string or hostname; returns RTT in ms or -1 on timeout
int net_ping(const char* target, int timeout_ms);

// Resolve hostname via DNS (or literal IPv4); returns packed IPv4 (a<<24|b<<16|c<<8|d) or 0 on fail
uint32_t net_dns_resolve(const char* host, int timeout_ms);

// Start a non-blocking HTTP/1.1 GET transfer (returns transfer ID >= 0 or -1 on error)
// If save_filename is non-NULL, streams body directly to \DOWNLOADS\<save_filename> on FAT32
int net_http_start(const char* url, const char* save_filename);

// Poll/inspect an active HTTP transfer by ID
const HttpTransfer* net_http_get(int transfer_id);

// Cancel or release an HTTP transfer slot
void net_http_close(int transfer_id);

// Download history list (\DOWNLOADS\ manifest & active downloads)
int net_download_count(void);
const DownloadEntry* net_get_download(int idx);
void net_clear_finished_downloads(void);

// Format IPv4 helper (packed uint32 -> "a.b.c.d")
void net_format_ip(uint32_t ip, char* out, int max_len);

#ifdef __cplusplus
}
#endif

#endif // BLANKOS_NET_H
