#include <efi.h>
#include <efilib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "net.h"

extern "C" {
    void* kmalloc(uint64_t size);
    void kfree(void* ptr);
    uint32_t pci_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
}

// ============================================================================
// Constants & Protocol Definitions
// ============================================================================

#define ARP_CACHE_SIZE       16
#define DNS_CACHE_SIZE       16
#define MAX_HTTP_SLOTS       4
#define MAX_DOWNLOADS        32
#define MAX_HTTP_BODY_MEM    (2 * 1024 * 1024)
#define RX_FRAMES_PER_TICK   16
#define TCP_RECV_WINDOW      16384

#define ETH_TYPE_IPV4        0x0800
#define ETH_TYPE_ARP         0x0806

#define IP_PROTO_ICMP        1
#define IP_PROTO_TCP         6
#define IP_PROTO_UDP         17

#define TCP_FIN              0x01
#define TCP_SYN              0x02
#define TCP_RST              0x04
#define TCP_PSH              0x08
#define TCP_ACK              0x10

#define SEQ_LT(a, b)         ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b)        ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)         ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b)        ((int32_t)((a) - (b)) >= 0)

#pragma pack(push, 1)

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;
} EthHdr;

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} ArpPkt;

typedef struct {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} Ipv4Hdr;

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} IcmpHdr;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} UdpHdr;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} TcpHdr;

typedef struct {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
} DhcpHdr;

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} DnsHdr;

#pragma pack(pop)

// ============================================================================
// Internal State
// ============================================================================

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    bool     valid;
    uint64_t timestamp_ms;
} ArpEntry;

typedef struct {
    char     host[128];
    uint32_t ip;
    bool     valid;
    uint64_t timestamp_ms;
} DnsEntry;

typedef enum {
    TCP_CLOSED = 0,
    TCP_WAIT_DNS = 1,
    TCP_WAIT_ARP = 2,
    TCP_SYN_SENT = 3,
    TCP_ESTABLISHED = 4,
    TCP_FIN_WAIT = 5
} TcpInternalPhase;

typedef struct {
    HttpTransfer     pub;
    bool             slot_used;
    TcpInternalPhase tcp_phase;
    uint32_t         remote_ip;
    uint16_t         local_port;
    uint32_t         snd_una;
    uint32_t         snd_nxt;
    uint32_t         rcv_nxt;
    bool             peer_fin;
    bool             fin_sent;
    uint16_t         dns_txid;
    uint16_t         dns_sport;
    uint64_t         phase_start_ms;
    uint64_t         last_activity_ms;
    uint64_t         transfer_start_ms;
    int              retries;
    bool             headers_parsed;
    bool             is_chunked;
    bool             request_sent;
    char             hdr_buf[4096];
    int              hdr_buf_len;
    char             redirect_location[512];
    int              redirect_count;
    int              chunk_state; // 0=size line, 1=data, 2=trailing CRLF, 3=done
    int              chunk_remaining;
    char             chunk_line[32];
    int              chunk_line_len;
    EFI_FILE_HANDLE  disk_file;
    int              dl_entry_idx;
} HttpSlot;

static EFI_SYSTEM_TABLE*          g_st = NULL;
static EFI_SIMPLE_NETWORK_PROTOCOL* g_snp = NULL;
static EFI_GUID                   g_snp_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
static EFI_GUID                   g_sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

static bool      g_initialized = false;
static NetStatus g_status;

static uint8_t   g_tx_ring[4][1600] __attribute__((aligned(16)));
static int       g_tx_idx = 0;
static uint8_t   g_rx_frame[2048] __attribute__((aligned(16)));
static uint8_t   g_ip_pkt_buf[1600] __attribute__((aligned(16)));

static uint64_t  g_tsc_boot = 0;
static uint64_t  g_tsc_per_ms = 1000000ULL;
static uint64_t  g_fallback_ms = 0;

static uint16_t  g_ip_id = 1;
static uint16_t  g_ephemeral_port = 49152;

static ArpEntry  g_arp_cache[ARP_CACHE_SIZE];
static DnsEntry  g_dns_cache[DNS_CACHE_SIZE];

// DHCP state
static uint32_t  g_dhcp_xid = 0;
static uint32_t  g_dhcp_seq = 0;
static bool      g_dhcp_offer_ok = false;
static bool      g_dhcp_ack_ok = false;
static uint32_t  g_dhcp_offered_ip = 0;
static uint32_t  g_dhcp_server_id = 0;
static uint32_t  g_dhcp_subnet = 0;
static uint32_t  g_dhcp_gw = 0;
static uint32_t  g_dhcp_dns = 0;

// ICMP Ping state
static uint16_t  g_ping_id = 0x424B;
static uint16_t  g_ping_seq = 0;
static bool      g_ping_waiting = false;
static bool      g_ping_reply_ok = false;
static uint32_t  g_ping_target_ip = 0;

// Sync DNS resolver state
static bool      g_sync_dns_waiting = false;
static bool      g_sync_dns_done = false;
static uint16_t  g_sync_dns_txid = 0;
static uint16_t  g_sync_dns_sport = 0;
static uint32_t  g_sync_dns_result = 0;

// HTTP & Download state
static HttpSlot      g_http_slots[MAX_HTTP_SLOTS];
static DownloadEntry g_downloads[MAX_DOWNLOADS];
static int           g_download_count = 0;

// ============================================================================
// Low-Level Freestanding Helpers
// ============================================================================

static inline uint16_t htons(uint16_t x) {
    return (uint16_t)(((x & 0x00FFU) << 8) | ((x & 0xFF00U) >> 8));
}

static inline uint16_t ntohs(uint16_t x) {
    return htons(x);
}

static inline uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFU) << 24) |
           ((x & 0x0000FF00U) << 8)  |
           ((x & 0x00FF0000U) >> 8)  |
           ((x & 0xFF000000U) >> 24);
}

static inline uint32_t ntohl(uint32_t x) {
    return htonl(x);
}

static inline uint64_t read_tsc(void) {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static uint64_t now_ms(void) {
    if (g_tsc_per_ms > 0) {
        uint64_t now = read_tsc();
        if (now >= g_tsc_boot) {
            return (now - g_tsc_boot) / g_tsc_per_ms;
        }
    }
    return g_fallback_ms;
}

static void stall_us(UINTN us) {
    if (g_st && g_st->BootServices) {
        g_st->BootServices->Stall(us);
    }
    g_fallback_ms += (uint64_t)(us / 1000);
}

static void n_memset(void* dst, uint8_t val, int len) {
    uint8_t* d = (uint8_t*)dst;
    for (int i = 0; i < len; i++) d[i] = val;
}

static void n_memcpy(void* dst, const void* src, int len) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (int i = 0; i < len; i++) d[i] = s[i];
}

static int n_memcmp(const void* a, const void* b, int len) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (int i = 0; i < len; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

static int n_strlen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void n_strcpy(char* dst, const char* src, int max_len) {
    if (!dst || max_len <= 0) return;
    int i = 0;
    while (src && src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static char to_lower_ch(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static bool n_streq_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    int i = 0;
    while (a[i] && b[i]) {
        if (to_lower_ch(a[i]) != to_lower_ch(b[i])) return false;
        i++;
    }
    return a[i] == b[i];
}

static bool n_starts_with_ci(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    int i = 0;
    while (prefix[i]) {
        if (!s[i] || to_lower_ch(s[i]) != to_lower_ch(prefix[i])) return false;
        i++;
    }
    return true;
}

static bool n_contains_ci(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && to_lower_ch(hay[i + j]) == to_lower_ch(needle[j])) {
            j++;
        }
        if (!needle[j]) return true;
    }
    return false;
}

static int n_atoi(const char* s) {
    if (!s) return 0;
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    int sign = 1;
    if (s[i] == '-') { sign = -1; i++; }
    else if (s[i] == '+') { i++; }
    int val = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        val = val * 10 + (s[i] - '0');
        i++;
    }
    return val * sign;
}

static int int_to_ascii(int val, char* out, int max_len) {
    if (!out || max_len <= 1) return 0;
    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    char tmp[16];
    int p = 0;
    bool neg = false;
    uint32_t u = (uint32_t)val;
    if (val < 0) {
        neg = true;
        u = (uint32_t)(-val);
    }
    while (u > 0 && p < 15) {
        tmp[p++] = (char)('0' + (u % 10));
        u /= 10;
    }
    int pos = 0;
    if (neg && pos < max_len - 1) out[pos++] = '-';
    while (p > 0 && pos < max_len - 1) {
        out[pos++] = tmp[--p];
    }
    out[pos] = '\0';
    return pos;
}

static void format_mac_str(const uint8_t mac[6], char* out, int max_len) {
    static const char hex[] = "0123456789ABCDEF";
    if (!out || max_len < 18) return;
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        out[pos++] = hex[(mac[i] >> 4) & 0xF];
        out[pos++] = hex[mac[i] & 0xF];
        if (i < 5) out[pos++] = ':';
    }
    out[pos] = '\0';
}

static uint32_t ip_pack(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

static bool parse_ipv4_literal(const char* s, uint32_t* out_ip) {
    if (!s || !s[0]) return false;
    uint32_t parts[4] = {0, 0, 0, 0};
    int idx = 0;
    int digits = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            parts[idx] = parts[idx] * 10U + (uint32_t)(c - '0');
            if (parts[idx] > 255U) return false;
            digits++;
        } else if (c == '.') {
            if (digits == 0 || idx >= 3) return false;
            idx++;
            digits = 0;
        } else {
            return false;
        }
    }
    if (idx != 3 || digits == 0) return false;
    *out_ip = ip_pack((uint8_t)parts[0], (uint8_t)parts[1], (uint8_t)parts[2], (uint8_t)parts[3]);
    return true;
}

static uint16_t checksum_16(const uint8_t* data, int len, uint32_t init_sum) {
    uint32_t sum = init_sum;
    int i = 0;
    while (i + 1 < len) {
        sum += ((uint32_t)data[i] << 8) | (uint32_t)data[i + 1];
        i += 2;
    }
    if (i < len) {
        sum += ((uint32_t)data[i] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFU) + (sum >> 16);
    }
    return htons((uint16_t)(~sum & 0xFFFFU));
}

static uint16_t tcp_udp_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto, const uint8_t* l4, int l4_len) {
    uint32_t pseudo = ((src_ip >> 16) & 0xFFFFU) + (src_ip & 0xFFFFU) +
                      ((dst_ip >> 16) & 0xFFFFU) + (dst_ip & 0xFFFFU) +
                      (uint32_t)proto + (uint32_t)l4_len;
    return checksum_16(l4, l4_len, pseudo);
}

// ============================================================================
// PCI Network Adapter Probe
// ============================================================================

static void probe_pci_nic_name(char* out_name, int max_len) {
    out_name[0] = '\0';
    char fallback[64];
    fallback[0] = '\0';

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_read_dword((uint8_t)bus, dev, 0, 0x00);
            uint16_t v0 = (uint16_t)(id0 & 0xFFFFU);
            if (v0 == 0xFFFFU || v0 == 0x0000U) continue;

            uint32_t hdr = pci_read_dword((uint8_t)bus, dev, 0, 0x0C);
            uint8_t max_func = (((hdr >> 16) & 0x80U) != 0) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                uint32_t id = (func == 0) ? id0 : pci_read_dword((uint8_t)bus, dev, func, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFFU);
                if (vendor == 0xFFFFU || vendor == 0x0000U) continue;
                uint16_t device = (uint16_t)((id >> 16) & 0xFFFFU);

                uint32_t cls = pci_read_dword((uint8_t)bus, dev, func, 0x08);
                uint8_t class_code = (uint8_t)((cls >> 24) & 0xFFU);
                uint8_t subclass   = (uint8_t)((cls >> 16) & 0xFFU);
                if (class_code != 0x02) continue;

                const char* matched = NULL;
                if (vendor == 0x8086) {
                    if (device == 0x100E || device == 0x100F || device == 0x10D3 || device == 0x10EA) {
                        matched = "Intel e1000 / PRO/1000";
                    } else if (device == 0x153A || device == 0x15B7 || device == 0x15B8) {
                        matched = "Intel Ethernet Connection";
                    } else {
                        matched = "Intel Gigabit Ethernet";
                    }
                } else if (vendor == 0x15AD) {
                    matched = (device == 0x07B0) ? "VMware VMXNET3" : "VMware VMXNET";
                } else if (vendor == 0x10EC) {
                    matched = (device == 0x8139) ? "Realtek RTL8139" : "Realtek RTL8168";
                } else if (vendor == 0x1AF4) {
                    matched = "VirtIO Net";
                } else if (vendor == 0x1022) {
                    matched = "AMD PCnet-PCI II";
                } else if (vendor == 0x14E4) {
                    matched = "Broadcom NetXtreme Ethernet";
                } else {
                    matched = (subclass == 0x00) ? "PCI Ethernet Controller" : "PCI Network Controller";
                }

                if (subclass == 0x00) {
                    n_strcpy(out_name, matched, max_len);
                    return;
                }
                if (fallback[0] == '\0') {
                    n_strcpy(fallback, matched, 64);
                }
            }
        }
    }

    if (fallback[0] != '\0') {
        n_strcpy(out_name, fallback, max_len);
    }
}

// ============================================================================
// UEFI FAT32 \DOWNLOADS Helpers
// ============================================================================

static void ascii_to_ucs2(const char* src, CHAR16* dst, int max_chars) {
    int i = 0;
    while (src && src[i] && i < max_chars - 1) {
        dst[i] = (CHAR16)(uint8_t)src[i];
        i++;
    }
    dst[i] = 0;
}

static void ucs2_to_ascii(const CHAR16* src, char* dst, int max_chars) {
    int i = 0;
    while (src && src[i] && i < max_chars - 1) {
        dst[i] = (char)(src[i] & 0x7FU);
        i++;
    }
    dst[i] = '\0';
}

static EFI_FILE_HANDLE open_downloads_file_for_write(const char* filename) {
    if (!g_st || !g_st->BootServices || !filename || !filename[0]) return NULL;

    UINTN count = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(g_st->BootServices->LocateHandleBuffer(ByProtocol, &g_sfs_guid, NULL, &count, &handles)) || !handles) {
        return NULL;
    }

    CHAR16 wname[128];
    ascii_to_ucs2(filename, wname, 128);

    EFI_FILE_HANDLE result = NULL;
    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(g_st->BootServices->HandleProtocol(handles[i], &g_sfs_guid, (VOID**)&fs)) || !fs) {
            continue;
        }
        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) {
            continue;
        }

        EFI_FILE_HANDLE dl_dir = NULL;
        EFI_STATUS dst = root->Open(
            root,
            &dl_dir,
            (CHAR16*)L"DOWNLOADS",
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
            EFI_FILE_DIRECTORY
        );
        if (EFI_ERROR(dst) || !dl_dir) {
            root->Close(root);
            continue;
        }

        // Remove any prior file of the same name so stale trailing bytes never remain
        EFI_FILE_HANDLE old_file = NULL;
        if (!EFI_ERROR(dl_dir->Open(dl_dir, &old_file, wname, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && old_file) {
            old_file->Delete(old_file);
        }

        EFI_FILE_HANDLE new_file = NULL;
        EFI_STATUS fst = dl_dir->Open(
            dl_dir,
            &new_file,
            wname,
            EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
            0
        );
        dl_dir->Close(dl_dir);
        root->Close(root);

        if (!EFI_ERROR(fst) && new_file) {
            result = new_file;
            break;
        }
    }

    g_st->BootServices->FreePool(handles);
    return result;
}

static void scan_existing_downloads(void) {
    if (!g_st || !g_st->BootServices) return;

    UINTN count = 0;
    EFI_HANDLE* handles = NULL;
    if (EFI_ERROR(g_st->BootServices->LocateHandleBuffer(ByProtocol, &g_sfs_guid, NULL, &count, &handles)) || !handles) {
        return;
    }

    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        if (EFI_ERROR(g_st->BootServices->HandleProtocol(handles[i], &g_sfs_guid, (VOID**)&fs)) || !fs) {
            continue;
        }
        EFI_FILE_HANDLE root = NULL;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) {
            continue;
        }

        EFI_FILE_HANDLE dl_dir = NULL;
        EFI_STATUS dst = root->Open(root, &dl_dir, (CHAR16*)L"DOWNLOADS", EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(dst) || !dl_dir) {
            root->Close(root);
            continue;
        }

        dl_dir->SetPosition(dl_dir, 0);
        uint8_t info_buf[512];
        while (g_download_count < MAX_DOWNLOADS) {
            UINTN sz = sizeof(info_buf);
            if (EFI_ERROR(dl_dir->Read(dl_dir, &sz, info_buf)) || sz == 0) break;

            EFI_FILE_INFO* info = (EFI_FILE_INFO*)info_buf;
            if ((info->Attribute & EFI_FILE_DIRECTORY) != 0) continue;
            if (info->FileName[0] == L'.' || info->FileName[0] == 0) continue;

            char fname[128];
            ucs2_to_ascii(info->FileName, fname, 128);
            if (!fname[0]) continue;

            bool dup = false;
            for (int k = 0; k < g_download_count; k++) {
                if (n_streq_ci(g_downloads[k].filename, fname)) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            DownloadEntry* de = &g_downloads[g_download_count++];
            n_memset(de, 0, sizeof(DownloadEntry));
            n_strcpy(de->filename, fname, 128);
            n_strcpy(de->disk_path, "\\DOWNLOADS\\", 192);
            int dp = n_strlen(de->disk_path);
            n_strcpy(de->disk_path + dp, fname, 192 - dp);
            de->total_bytes = (int)info->FileSize;
            de->downloaded_bytes = (int)info->FileSize;
            de->state = HTTP_DONE;
            de->saved_ok = true;
            n_strcpy(de->status_text, "On Disk", 64);
        }

        dl_dir->Close(dl_dir);
        root->Close(root);
    }

    g_st->BootServices->FreePool(handles);
}

// ============================================================================
// Ethernet / ARP / IPv4 TX
// ============================================================================

static bool eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const uint8_t* payload, int payload_len) {
    if (!g_snp || !g_status.nic_present) return false;
    if (payload_len < 0 || 14 + payload_len > 1514) return false;

    for (int i = 0; i < 4; i++) {
        UINT32 intr = 0;
        VOID* tx_done = NULL;
        g_snp->GetStatus(g_snp, &intr, &tx_done);
        if (!tx_done) break;
    }

    uint8_t* frame = g_tx_ring[g_tx_idx];
    g_tx_idx = (g_tx_idx + 1) & 3;

    EthHdr* eth = (EthHdr*)frame;
    n_memcpy(eth->dst, dst_mac, 6);
    n_memcpy(eth->src, g_status.mac, 6);
    eth->ethertype = htons(ethertype);
    if (payload_len > 0) {
        n_memcpy(frame + 14, payload, payload_len);
    }

    int wire_len = 14 + payload_len;
    if (wire_len < 60) {
        n_memset(frame + wire_len, 0, 60 - wire_len);
        wire_len = 60;
    }

    EFI_STATUS st = EFI_NOT_READY;
    for (int attempt = 0; attempt < 8; attempt++) {
        st = g_snp->Transmit(g_snp, 0, (UINTN)wire_len, (VOID*)frame, NULL, NULL, NULL);
        if (st == EFI_SUCCESS) break;
        UINT32 intr = 0;
        VOID* tx_done = NULL;
        g_snp->GetStatus(g_snp, &intr, &tx_done);
        stall_us(150);
    }
    if (EFI_ERROR(st)) return false;

    g_status.tx_packets++;
    g_status.tx_bytes += (uint64_t)wire_len;

    for (int poll = 0; poll < 10; poll++) {
        UINT32 intr = 0;
        VOID* tx_done = NULL;
        g_snp->GetStatus(g_snp, &intr, &tx_done);
        if (tx_done != NULL) break;
        stall_us(50);
    }
    return true;
}

static bool arp_lookup(uint32_t ip, uint8_t out_mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            n_memcpy(out_mac, g_arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_learn(uint32_t ip, const uint8_t mac[6]) {
    if (ip == 0 || ip == 0xFFFFFFFFU) return;
    int oldest_idx = 0;
    uint64_t oldest_ts = ~0ULL;

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
            n_memcpy(g_arp_cache[i].mac, mac, 6);
            g_arp_cache[i].timestamp_ms = now_ms();
            return;
        }
        if (!g_arp_cache[i].valid) {
            oldest_idx = i;
            oldest_ts = 0;
            break;
        }
        if (g_arp_cache[i].timestamp_ms < oldest_ts) {
            oldest_ts = g_arp_cache[i].timestamp_ms;
            oldest_idx = i;
        }
    }

    g_arp_cache[oldest_idx].ip = ip;
    n_memcpy(g_arp_cache[oldest_idx].mac, mac, 6);
    g_arp_cache[oldest_idx].valid = true;
    g_arp_cache[oldest_idx].timestamp_ms = now_ms();
}

static bool arp_send_packet(uint16_t oper, const uint8_t target_mac[6], uint32_t target_ip) {
    ArpPkt pkt;
    pkt.htype = htons(1);
    pkt.ptype = htons(ETH_TYPE_IPV4);
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.oper = htons(oper);
    n_memcpy(pkt.sha, g_status.mac, 6);
    pkt.spa = htonl(g_status.ip_addr);
    n_memcpy(pkt.tha, target_mac, 6);
    pkt.tpa = htonl(target_ip);

    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t* eth_dst = (oper == 1) ? bcast : target_mac;
    return eth_send(eth_dst, ETH_TYPE_ARP, (const uint8_t*)&pkt, sizeof(ArpPkt));
}

static bool arp_send_request(uint32_t target_ip) {
    static const uint8_t zero_mac[6] = {0, 0, 0, 0, 0, 0};
    return arp_send_packet(1, zero_mac, target_ip);
}

static uint32_t ip_next_hop(uint32_t dst_ip) {
    if (dst_ip == 0xFFFFFFFFU) return 0xFFFFFFFFU;
    if (g_status.ip_addr != 0 && g_status.subnet_mask != 0) {
        if ((dst_ip & g_status.subnet_mask) == (g_status.ip_addr & g_status.subnet_mask)) {
            if ((dst_ip | g_status.subnet_mask) == 0xFFFFFFFFU) return 0xFFFFFFFFU;
            return dst_ip;
        }
    }
    if (g_status.gateway_ip != 0) {
        return g_status.gateway_ip;
    }
    return dst_ip;
}

static bool arp_resolve_blocking(uint32_t ip, uint8_t out_mac[6], int timeout_ms) {
    uint32_t nh = ip_next_hop(ip);
    if (nh == 0xFFFFFFFFU) {
        for (int i = 0; i < 6; i++) out_mac[i] = 0xFF;
        return true;
    }
    if (arp_lookup(nh, out_mac)) return true;

    uint64_t start = now_ms();
    int retries = 0;
    arp_send_request(nh);
    while ((int)(now_ms() - start) < timeout_ms && retries < timeout_ms / 2) {
        net_tick();
        if (arp_lookup(nh, out_mac)) return true;
        stall_us(5000);
        retries++;
        if (retries % 40 == 0) {
            arp_send_request(nh);
        }
    }
    return false;
}

static bool ip_send(uint32_t dst_ip, uint8_t proto, const uint8_t* payload, int payload_len) {
    if (!g_snp || !g_status.nic_present) return false;
    if (payload_len < 0 || 20 + payload_len > 1500) return false;

    uint8_t dst_mac[6];
    uint32_t nh = ip_next_hop(dst_ip);
    if (nh == 0xFFFFFFFFU) {
        for (int i = 0; i < 6; i++) dst_mac[i] = 0xFF;
    } else if (!arp_lookup(nh, dst_mac)) {
        arp_send_request(nh);
        return false;
    }

    Ipv4Hdr* ip = (Ipv4Hdr*)g_ip_pkt_buf;
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->tot_len = htons((uint16_t)(20 + payload_len));
    ip->id = htons(g_ip_id++);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    ip->src_ip = htonl(g_status.ip_addr);
    ip->dst_ip = htonl(dst_ip);
    ip->checksum = checksum_16(g_ip_pkt_buf, 20, 0);

    if (payload_len > 0) {
        n_memcpy(g_ip_pkt_buf + 20, payload, payload_len);
    }

    return eth_send(dst_mac, ETH_TYPE_IPV4, g_ip_pkt_buf, 20 + payload_len);
}

static bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const uint8_t* data, int data_len) {
    if (data_len < 0 || 8 + data_len > 1480) return false;
    uint8_t udp_buf[1480];
    UdpHdr* udp = (UdpHdr*)udp_buf;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons((uint16_t)(8 + data_len));
    udp->checksum = 0;
    if (data_len > 0) {
        n_memcpy(udp_buf + 8, data, data_len);
    }
    uint16_t csum = tcp_udp_checksum(g_status.ip_addr, dst_ip, IP_PROTO_UDP, udp_buf, 8 + data_len);
    if (csum == 0) csum = 0xFFFFU;
    udp->checksum = csum;
    return ip_send(dst_ip, IP_PROTO_UDP, udp_buf, 8 + data_len);
}

static bool tcp_send_segment(
    uint32_t dst_ip,
    uint16_t src_port,
    uint16_t dst_port,
    uint32_t seq,
    uint32_t ack,
    uint8_t flags,
    bool send_mss,
    const uint8_t* data,
    int data_len
) {
    int opt_len = send_mss ? 4 : 0;
    int hdr_len = 20 + opt_len;
    if (data_len < 0 || hdr_len + data_len > 1480) return false;

    uint8_t tcp_buf[1480];
    TcpHdr* tcp = (TcpHdr*)tcp_buf;
    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->seq = htonl(seq);
    tcp->ack = htonl(ack);
    tcp->data_off = (uint8_t)((hdr_len / 4) << 4);
    tcp->flags = flags;
    tcp->window = htons(TCP_RECV_WINDOW);
    tcp->checksum = 0;
    tcp->urgent = 0;

    if (send_mss) {
        tcp_buf[20] = 2;    // MSS option kind
        tcp_buf[21] = 4;    // length
        tcp_buf[22] = 0x05; // 1460 = 0x05B4
        tcp_buf[23] = 0xB4;
    }

    if (data_len > 0 && data) {
        n_memcpy(tcp_buf + hdr_len, data, data_len);
    }

    tcp->checksum = tcp_udp_checksum(g_status.ip_addr, dst_ip, IP_PROTO_TCP, tcp_buf, hdr_len + data_len);
    return ip_send(dst_ip, IP_PROTO_TCP, tcp_buf, hdr_len + data_len);
}

// ============================================================================
// DNS Helpers (Encode Query, Parse Answer with 0xC0xx Compression)
// ============================================================================

static bool dns_cache_lookup(const char* host, uint32_t* out_ip) {
    uint64_t t = now_ms();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (g_dns_cache[i].valid && n_streq_ci(g_dns_cache[i].host, host)) {
            if (t - g_dns_cache[i].timestamp_ms < 300000ULL) {
                *out_ip = g_dns_cache[i].ip;
                return true;
            }
            g_dns_cache[i].valid = false;
        }
    }
    return false;
}

static void dns_cache_store(const char* host, uint32_t ip) {
    if (!host || !host[0] || ip == 0) return;
    int slot = 0;
    uint64_t oldest = ~0ULL;
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (g_dns_cache[i].valid && n_streq_ci(g_dns_cache[i].host, host)) {
            slot = i;
            break;
        }
        if (!g_dns_cache[i].valid) {
            slot = i;
            break;
        }
        if (g_dns_cache[i].timestamp_ms < oldest) {
            oldest = g_dns_cache[i].timestamp_ms;
            slot = i;
        }
    }
    n_strcpy(g_dns_cache[slot].host, host, 128);
    g_dns_cache[slot].ip = ip;
    g_dns_cache[slot].valid = true;
    g_dns_cache[slot].timestamp_ms = now_ms();
}

static bool send_dns_a_query(const char* host, uint16_t txid, uint16_t sport) {
    if (!host || !host[0]) return false;
    uint32_t dns_srv = g_status.dns_ip ? g_status.dns_ip : ip_pack(8, 8, 8, 8);

    uint8_t pkt[512];
    n_memset(pkt, 0, sizeof(pkt));
    DnsHdr* dns = (DnsHdr*)pkt;
    dns->id = htons(txid);
    dns->flags = htons(0x0100U); // Standard query, Recursion Desired
    dns->qdcount = htons(1);

    int pos = sizeof(DnsHdr);
    int i = 0;
    while (host[i] && pos < 480) {
        int label_len_pos = pos++;
        int label_len = 0;
        while (host[i] && host[i] != '.' && pos < 480 && label_len < 63) {
            pkt[pos++] = (uint8_t)host[i++];
            label_len++;
        }
        pkt[label_len_pos] = (uint8_t)label_len;
        if (host[i] == '.') i++;
    }
    pkt[pos++] = 0; // Root label

    // QTYPE = A (1), QCLASS = IN (1)
    pkt[pos++] = 0; pkt[pos++] = 1;
    pkt[pos++] = 0; pkt[pos++] = 1;

    return udp_send(dns_srv, sport, 53, pkt, pos);
}

static int dns_skip_name(const uint8_t* pkt, int pkt_len, int offset) {
    for (int steps = 0; steps < 128; steps++) {
        if (offset < 0 || offset >= pkt_len) return -1;
        uint8_t len = pkt[offset];
        if (len == 0) {
            return offset + 1;
        }
        if ((len & 0xC0U) == 0xC0U) {
            if (offset + 1 >= pkt_len) return -1;
            return offset + 2;
        }
        offset += 1 + (int)len;
    }
    return -1;
}

static uint32_t dns_parse_a_response(const uint8_t* pkt, int pkt_len, uint16_t expected_id) {
    if (pkt_len < (int)sizeof(DnsHdr)) return 0;
    const DnsHdr* dns = (const DnsHdr*)pkt;
    if (ntohs(dns->id) != expected_id) return 0;
    uint16_t flags = ntohs(dns->flags);
    if ((flags & 0x8000U) == 0) return 0;       // Not a response
    if ((flags & 0x000FU) != 0) return 0;       // Error RCODE

    uint16_t qd = ntohs(dns->qdcount);
    uint16_t an = ntohs(dns->ancount);
    int offset = (int)sizeof(DnsHdr);

    for (uint16_t q = 0; q < qd; q++) {
        offset = dns_skip_name(pkt, pkt_len, offset);
        if (offset < 0 || offset + 4 > pkt_len) return 0;
        offset += 4;
    }

    for (uint16_t a = 0; a < an; a++) {
        offset = dns_skip_name(pkt, pkt_len, offset);
        if (offset < 0 || offset + 10 > pkt_len) return 0;

        uint16_t rtype  = (uint16_t)(((uint16_t)pkt[offset] << 8) | pkt[offset + 1]);
        uint16_t rclass = (uint16_t)(((uint16_t)pkt[offset + 2] << 8) | pkt[offset + 3]);
        uint16_t rdlen  = (uint16_t)(((uint16_t)pkt[offset + 8] << 8) | pkt[offset + 9]);
        offset += 10;
        if (offset + rdlen > pkt_len) return 0;

        if (rtype == 1 && rclass == 1 && rdlen == 4) {
            return ip_pack(pkt[offset], pkt[offset + 1], pkt[offset + 2], pkt[offset + 3]);
        }
        offset += rdlen;
    }
    return 0;
}

// ============================================================================
// DHCP Client TX/RX
// ============================================================================

static bool dhcp_send_msg(uint8_t msg_type, uint32_t req_ip, uint32_t server_id) {
    uint8_t pkt[360];
    n_memset(pkt, 0, sizeof(pkt));

    DhcpHdr* dhcp = (DhcpHdr*)pkt;
    dhcp->op = 1;    // BOOTREQUEST
    dhcp->htype = 1; // Ethernet
    dhcp->hlen = 6;
    dhcp->hops = 0;
    dhcp->xid = htonl(g_dhcp_xid);
    dhcp->secs = 0;
    dhcp->flags = htons(0x8000U); // Broadcast flag
    n_memcpy(dhcp->chaddr, g_status.mac, 6);
    dhcp->magic = htonl(0x63825363U);

    int pos = sizeof(DhcpHdr);
    // Option 53: DHCP Message Type
    pkt[pos++] = 53; pkt[pos++] = 1; pkt[pos++] = msg_type;

    if (msg_type == 3) { // DHCPREQUEST
        pkt[pos++] = 50; pkt[pos++] = 4;
        uint32_t rip = htonl(req_ip);
        n_memcpy(&pkt[pos], &rip, 4); pos += 4;

        if (server_id != 0) {
            pkt[pos++] = 54; pkt[pos++] = 4;
            uint32_t sid = htonl(server_id);
            n_memcpy(&pkt[pos], &sid, 4); pos += 4;
        }
    }

    // Option 61: Client Identifier
    pkt[pos++] = 61; pkt[pos++] = 7; pkt[pos++] = 1;
    n_memcpy(&pkt[pos], g_status.mac, 6); pos += 6;

    // Option 55: Parameter Request List (Subnet Mask, Router, DNS, Domain)
    pkt[pos++] = 55; pkt[pos++] = 4;
    pkt[pos++] = 1; pkt[pos++] = 3; pkt[pos++] = 6; pkt[pos++] = 15;

    // Option 255: End
    pkt[pos++] = 255;

    return udp_send(0xFFFFFFFFU, 68, 67, pkt, pos);
}

static void dhcp_handle_rx(uint32_t src_ip, const uint8_t* payload, int len) {
    if (len < (int)sizeof(DhcpHdr)) return;
    const DhcpHdr* dhcp = (const DhcpHdr*)payload;
    if (dhcp->op != 2) return; // BOOTREPLY
    if (ntohl(dhcp->xid) != g_dhcp_xid) return;
    if (ntohl(dhcp->magic) != 0x63825363U) return;
    if (n_memcmp(dhcp->chaddr, g_status.mac, 6) != 0) return;

    uint8_t msg_type = 0;
    uint32_t subnet = 0;
    uint32_t router = 0;
    uint32_t dns = 0;
    uint32_t server_id = 0;

    int pos = (int)sizeof(DhcpHdr);
    while (pos < len) {
        uint8_t opt = payload[pos++];
        if (opt == 0) continue;   // Pad
        if (opt == 255) break;    // End
        if (pos >= len) break;
        uint8_t olen = payload[pos++];
        if (pos + olen > len) break;

        if (opt == 53 && olen >= 1) {
            msg_type = payload[pos];
        } else if (opt == 1 && olen >= 4) {
            uint32_t v = 0; n_memcpy(&v, &payload[pos], 4);
            subnet = ntohl(v);
        } else if (opt == 3 && olen >= 4) {
            uint32_t v = 0; n_memcpy(&v, &payload[pos], 4);
            router = ntohl(v);
        } else if (opt == 6 && olen >= 4) {
            uint32_t v = 0; n_memcpy(&v, &payload[pos], 4);
            dns = ntohl(v);
        } else if (opt == 54 && olen >= 4) {
            uint32_t v = 0; n_memcpy(&v, &payload[pos], 4);
            server_id = ntohl(v);
        }
        pos += olen;
    }

    uint32_t yiaddr = ntohl(dhcp->yiaddr);
    if (server_id == 0) server_id = src_ip;

    if (msg_type == 2 && yiaddr != 0 && !g_dhcp_offer_ok) {
        g_dhcp_offered_ip = yiaddr;
        g_dhcp_server_id = server_id;
        g_dhcp_subnet = subnet ? subnet : ip_pack(255, 255, 255, 0);
        g_dhcp_gw = router;
        g_dhcp_dns = dns ? dns : ip_pack(8, 8, 8, 8);
        g_dhcp_offer_ok = true;
    } else if (msg_type == 5 && (yiaddr != 0 || g_dhcp_offered_ip != 0)) {
        if (yiaddr != 0) g_dhcp_offered_ip = yiaddr;
        if (subnet) g_dhcp_subnet = subnet;
        if (router) g_dhcp_gw = router;
        if (dns) g_dhcp_dns = dns;
        g_dhcp_ack_ok = true;
    }
}

// ============================================================================
// HTTP State Machine Helpers
// ============================================================================

static bool parse_http_url(const char* url, bool* out_https, char* out_host, int host_max, uint16_t* out_port, char* out_path, int path_max) {
    if (!url || !url[0]) return false;
    int i = 0;
    while (url[i] == ' ' || url[i] == '\t') i++;

    *out_https = false;
    *out_port = 80;

    if (n_starts_with_ci(url + i, "https://")) {
        *out_https = true;
        *out_port = 443;
        i += 8;
    } else if (n_starts_with_ci(url + i, "http://")) {
        i += 7;
    }

    int h = 0;
    while (url[i] && url[i] != ':' && url[i] != '/' && url[i] != '?' && url[i] != '#') {
        if (h < host_max - 1) out_host[h++] = url[i];
        i++;
    }
    out_host[h] = '\0';
    if (h == 0) return false;

    if (url[i] == ':') {
        i++;
        int p = 0;
        while (url[i] >= '0' && url[i] <= '9') {
            p = p * 10 + (url[i] - '0');
            i++;
        }
        if (p > 0 && p <= 65535) {
            *out_port = (uint16_t)p;
        }
    }

    if (url[i] == '/') {
        int p = 0;
        while (url[i] && url[i] != '#' && p < path_max - 1) {
            out_path[p++] = url[i++];
        }
        out_path[p] = '\0';
    } else if (url[i] == '?') {
        out_path[0] = '/';
        int p = 1;
        while (url[i] && url[i] != '#' && p < path_max - 1) {
            out_path[p++] = url[i++];
        }
        out_path[p] = '\0';
    } else {
        out_path[0] = '/';
        out_path[1] = '\0';
    }
    return true;
}

static void sync_download_entry(HttpSlot* s, const char* status_text, bool saved_ok) {
    if (!s || s->dl_entry_idx < 0 || s->dl_entry_idx >= g_download_count) return;
    DownloadEntry* de = &g_downloads[s->dl_entry_idx];
    de->total_bytes = (s->pub.content_length > 0) ? s->pub.content_length : s->pub.bytes_received;
    de->downloaded_bytes = s->pub.bytes_received;
    de->speed_bps = s->pub.speed_bps;
    de->state = s->pub.state;
    de->saved_ok = saved_ok;
    if (status_text) {
        n_strcpy(de->status_text, status_text, 64);
    }
}

static void close_slot_disk_file(HttpSlot* s) {
    if (s && s->disk_file) {
        s->disk_file->Flush(s->disk_file);
        s->disk_file->Close(s->disk_file);
        s->disk_file = NULL;
    }
}

static void http_fail_slot(HttpSlot* s, const char* err) {
    close_slot_disk_file(s);
    s->pub.state = HTTP_ERROR;
    s->tcp_phase = TCP_CLOSED;
    n_strcpy(s->pub.error_msg, err ? err : "Transfer error", 128);
    sync_download_entry(s, s->pub.error_msg, false);
}

static void http_complete_slot(HttpSlot* s) {
    if (s->pub.state == HTTP_DONE || s->pub.state == HTTP_ERROR) return;

    if (!s->fin_sent && s->tcp_phase == TCP_ESTABLISHED && s->remote_ip != 0) {
        tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_FIN | TCP_ACK, false, NULL, 0);
        s->snd_nxt++;
        s->fin_sent = true;
    }

    bool saved_ok = false;
    if (s->pub.save_to_disk) {
        if (s->disk_file != NULL) {
            close_slot_disk_file(s);
            saved_ok = true;
        }
    }

    s->tcp_phase = TCP_CLOSED;
    s->pub.state = HTTP_DONE;
    sync_download_entry(s, saved_ok ? "Saved to \\DOWNLOADS" : "Completed", saved_ok);
}

static bool http_grow_body_buf(HttpSlot* s, int needed_total) {
    if (needed_total > MAX_HTTP_BODY_MEM) return false;
    if (s->pub.body_buf && needed_total + 1 <= s->pub.body_capacity) return true;

    int new_cap = s->pub.body_capacity ? s->pub.body_capacity : 8192;
    while (new_cap < needed_total + 1) {
        new_cap *= 2;
    }
    if (new_cap > MAX_HTTP_BODY_MEM + 1) {
        new_cap = MAX_HTTP_BODY_MEM + 1;
    }

    char* new_buf = (char*)kmalloc((uint64_t)new_cap);
    if (!new_buf) return false;

    if (s->pub.body_buf) {
        if (s->pub.bytes_received > 0) {
            n_memcpy(new_buf, s->pub.body_buf, s->pub.bytes_received);
        }
        kfree(s->pub.body_buf);
    }
    s->pub.body_buf = new_buf;
    s->pub.body_capacity = new_cap;
    return true;
}

static void http_emit_body_bytes(HttpSlot* s, const uint8_t* data, int len) {
    if (!data || len <= 0) return;

    if (s->pub.save_to_disk && s->disk_file != NULL) {
        UINTN wsz = (UINTN)len;
        EFI_STATUS wst = s->disk_file->Write(s->disk_file, &wsz, (VOID*)data);
        if (EFI_ERROR(wst)) {
            http_fail_slot(s, "Disk write error");
            return;
        }
        // Keep a small tail preview (up to 4096 bytes) if buffer allocated
        s->pub.bytes_received += (int)wsz;
    } else {
        int avail = MAX_HTTP_BODY_MEM - s->pub.bytes_received;
        if (avail <= 0) return;
        int copy_len = (len <= avail) ? len : avail;
        if (!http_grow_body_buf(s, s->pub.bytes_received + copy_len)) {
            if (!s->pub.save_to_disk) {
                http_fail_slot(s, "Out of kernel heap memory");
            }
            return;
        }
        n_memcpy(s->pub.body_buf + s->pub.bytes_received, data, copy_len);
        s->pub.bytes_received += copy_len;
        s->pub.body_buf[s->pub.bytes_received] = '\0';
    }

    uint64_t elapsed = now_ms() - s->transfer_start_ms;
    if (elapsed == 0) elapsed = 1;
    s->pub.speed_bps = (int)(((uint64_t)s->pub.bytes_received * 1000ULL) / elapsed);
    sync_download_entry(s, "Downloading...", false);
}

static int parse_hex_int(const char* s, int len) {
    int val = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == ';' || c == ' ' || c == '\r' || c == '\n') break;
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else break;
        val = (val << 4) | d;
    }
    return val;
}

static void http_append_body(HttpSlot* s, const uint8_t* data, int len) {
    if (len <= 0 || s->pub.state == HTTP_DONE || s->pub.state == HTTP_ERROR) return;

    if (!s->is_chunked) {
        int take = len;
        if (s->pub.content_length >= 0) {
            int rem = s->pub.content_length - s->pub.bytes_received;
            if (rem <= 0) {
                http_complete_slot(s);
                return;
            }
            if (take > rem) take = rem;
        }
        http_emit_body_bytes(s, data, take);
        if (s->pub.state == HTTP_ERROR) return;
        if (s->pub.content_length >= 0 && s->pub.bytes_received >= s->pub.content_length) {
            http_complete_slot(s);
        }
        return;
    }

    // Chunked transfer decoder
    int pos = 0;
    while (pos < len && s->chunk_state != 3 && s->pub.state != HTTP_ERROR) {
        if (s->chunk_state == 0) {
            char c = (char)data[pos++];
            if (c == '\n') {
                s->chunk_line[s->chunk_line_len] = '\0';
                int csz = parse_hex_int(s->chunk_line, s->chunk_line_len);
                s->chunk_line_len = 0;
                if (csz == 0) {
                    s->chunk_state = 3;
                    http_complete_slot(s);
                    return;
                }
                s->chunk_remaining = csz;
                s->chunk_state = 1;
            } else if (c != '\r') {
                if (s->chunk_line_len < 30) {
                    s->chunk_line[s->chunk_line_len++] = c;
                }
            }
        } else if (s->chunk_state == 1) {
            int avail = len - pos;
            int take = (avail < s->chunk_remaining) ? avail : s->chunk_remaining;
            http_emit_body_bytes(s, data + pos, take);
            if (s->pub.state == HTTP_ERROR) return;
            pos += take;
            s->chunk_remaining -= take;
            if (s->chunk_remaining == 0) {
                s->chunk_state = 2;
            }
        } else if (s->chunk_state == 2) {
            char c = (char)data[pos++];
            if (c == '\n') {
                s->chunk_state = 0;
                s->chunk_line_len = 0;
            }
        }
    }
}

static void http_parse_headers(HttpSlot* s) {
    s->hdr_buf[s->hdr_buf_len] = '\0';
    s->pub.status_code = 0;
    s->pub.content_length = -1;
    s->pub.content_type[0] = '\0';
    s->redirect_location[0] = '\0';
    s->is_chunked = false;

    int pos = 0;
    // Status line: HTTP/1.x <code> ...
    while (pos < s->hdr_buf_len && s->hdr_buf[pos] != ' ' && s->hdr_buf[pos] != '\n') pos++;
    while (pos < s->hdr_buf_len && s->hdr_buf[pos] == ' ') pos++;
    s->pub.status_code = n_atoi(s->hdr_buf + pos);
    while (pos < s->hdr_buf_len && s->hdr_buf[pos] != '\n') pos++;
    if (pos < s->hdr_buf_len && s->hdr_buf[pos] == '\n') pos++;

    // Header lines
    while (pos < s->hdr_buf_len) {
        int line_start = pos;
        while (pos < s->hdr_buf_len && s->hdr_buf[pos] != '\n') pos++;
        int line_end = pos;
        if (pos < s->hdr_buf_len && s->hdr_buf[pos] == '\n') pos++;

        while (line_end > line_start && (s->hdr_buf[line_end - 1] == '\r' || s->hdr_buf[line_end - 1] == ' ')) {
            line_end--;
        }
        if (line_end <= line_start) break;

        s->hdr_buf[line_end] = '\0';
        char* line = s->hdr_buf + line_start;
        int colon = -1;
        for (int k = 0; line[k]; k++) {
            if (line[k] == ':') { colon = k; break; }
        }
        if (colon <= 0) continue;

        line[colon] = '\0';
        char* key = line;
        char* val = line + colon + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (n_streq_ci(key, "Content-Length")) {
            s->pub.content_length = n_atoi(val);
        } else if (n_streq_ci(key, "Content-Type")) {
            n_strcpy(s->pub.content_type, val, 64);
        } else if (n_streq_ci(key, "Transfer-Encoding")) {
            if (n_contains_ci(val, "chunked")) {
                s->is_chunked = true;
            }
        } else if (n_streq_ci(key, "Location")) {
            n_strcpy(s->redirect_location, val, 512);
        }
    }
}

static bool http_begin_redirect(HttpSlot* s) {
    if (s->redirect_count >= 5 || !s->redirect_location[0]) return false;

    if (!s->fin_sent && s->remote_ip != 0) {
        tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_FIN | TCP_ACK, false, NULL, 0);
        s->fin_sent = true;
    }

    char next_url[512];
    if (n_starts_with_ci(s->redirect_location, "https://")) {
        s->pub.is_https = true;
        http_fail_slot(s, "HTTPS requires TLS; use HTTP mirror");
        return true;
    } else if (n_starts_with_ci(s->redirect_location, "http://")) {
        n_strcpy(next_url, s->redirect_location, 512);
    } else if (s->redirect_location[0] == '/') {
        int p = 0;
        n_strcpy(next_url, "http://", 512);
        p = n_strlen(next_url);
        n_strcpy(next_url + p, s->pub.host, 512 - p);
        p = n_strlen(next_url);
        if (s->pub.port != 80 && p < 500) {
            next_url[p++] = ':';
            int_to_ascii((int)s->pub.port, next_url + p, 512 - p);
            p = n_strlen(next_url);
        }
        n_strcpy(next_url + p, s->redirect_location, 512 - p);
    } else {
        return false;
    }

    bool dummy_https = false;
    char host[128];
    char path[384];
    uint16_t port = 80;
    if (!parse_http_url(next_url, &dummy_https, host, 128, &port, path, 384)) {
        return false;
    }

    s->redirect_count++;
    n_strcpy(s->pub.url, next_url, 512);
    n_strcpy(s->pub.host, host, 128);
    n_strcpy(s->pub.path, path, 384);
    s->pub.port = port;
    s->pub.status_code = 0;
    s->pub.content_length = -1;
    s->pub.bytes_received = 0;
    s->pub.header_len = 0;
    s->pub.content_type[0] = '\0';
    s->headers_parsed = false;
    s->is_chunked = false;
    s->request_sent = false;
    s->hdr_buf_len = 0;
    s->chunk_state = 0;
    s->chunk_remaining = 0;
    s->chunk_line_len = 0;
    s->peer_fin = false;
    s->fin_sent = false;
    s->retries = 0;
    s->remote_ip = 0;
    s->local_port = g_ephemeral_port++;
    if (g_ephemeral_port < 49152) g_ephemeral_port = 49152;

    s->pub.state = HTTP_RESOLVING_DNS;
    s->tcp_phase = TCP_WAIT_DNS;
    s->phase_start_ms = now_ms();
    s->last_activity_ms = s->phase_start_ms;

    uint32_t cached_ip = 0;
    if (parse_ipv4_literal(s->pub.host, &cached_ip) || dns_cache_lookup(s->pub.host, &cached_ip)) {
        s->remote_ip = cached_ip;
        s->pub.state = HTTP_CONNECTING;
        s->tcp_phase = TCP_WAIT_ARP;
    } else {
        s->dns_txid = (uint16_t)(0x4B00U + s->pub.id * 37U + (uint16_t)(now_ms() & 0xFFU));
        s->dns_sport = g_ephemeral_port++;
        send_dns_a_query(s->pub.host, s->dns_txid, s->dns_sport);
    }
    return true;
}

static void http_send_get_request(HttpSlot* s) {
    char req[768];
    int p = 0;
    const char* m = "GET ";
    n_strcpy(req + p, m, 768 - p); p = n_strlen(req);
    n_strcpy(req + p, s->pub.path[0] ? s->pub.path : "/", 768 - p); p = n_strlen(req);
    n_strcpy(req + p, " HTTP/1.1\r\nHost: ", 768 - p); p = n_strlen(req);
    n_strcpy(req + p, s->pub.host, 768 - p); p = n_strlen(req);
    if (s->pub.port != 80 && p < 740) {
        req[p++] = ':';
        int_to_ascii((int)s->pub.port, req + p, 768 - p);
        p = n_strlen(req);
    }
    n_strcpy(req + p, "\r\nUser-Agent: BlankOS-Horizon/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n", 768 - p);
    p = n_strlen(req);

    tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_una, s->rcv_nxt, TCP_PSH | TCP_ACK, false, (const uint8_t*)req, p);
    if (!s->request_sent) {
        s->snd_nxt = s->snd_una + (uint32_t)p;
        s->request_sent = true;
        s->transfer_start_ms = now_ms();
    }
    s->pub.state = HTTP_SENDING;
    s->phase_start_ms = now_ms();
    s->last_activity_ms = s->phase_start_ms;
}

static void http_process_rx_bytes(HttpSlot* s, const uint8_t* data, int len) {
    if (!s || len <= 0 || s->pub.state == HTTP_DONE || s->pub.state == HTTP_ERROR) return;

    if (!s->headers_parsed) {
        int pos = 0;
        while (pos < len && !s->headers_parsed) {
            if (s->hdr_buf_len < (int)sizeof(s->hdr_buf) - 1) {
                s->hdr_buf[s->hdr_buf_len++] = (char)data[pos++];
            } else {
                http_fail_slot(s, "HTTP response header too large");
                return;
            }

            if (s->hdr_buf_len >= 4 &&
                s->hdr_buf[s->hdr_buf_len - 4] == '\r' &&
                s->hdr_buf[s->hdr_buf_len - 3] == '\n' &&
                s->hdr_buf[s->hdr_buf_len - 2] == '\r' &&
                s->hdr_buf[s->hdr_buf_len - 1] == '\n') {
                s->headers_parsed = true;
                s->pub.header_len = s->hdr_buf_len;
            } else if (s->hdr_buf_len >= 2 &&
                       s->hdr_buf[s->hdr_buf_len - 2] == '\n' &&
                       s->hdr_buf[s->hdr_buf_len - 1] == '\n') {
                s->headers_parsed = true;
                s->pub.header_len = s->hdr_buf_len;
            }
        }

        if (!s->headers_parsed) return;

        http_parse_headers(s);
        if ((s->pub.status_code == 301 || s->pub.status_code == 302 ||
             s->pub.status_code == 303 || s->pub.status_code == 307 ||
             s->pub.status_code == 308) && s->redirect_location[0] != '\0') {
            if (http_begin_redirect(s)) return;
        }

        s->pub.state = HTTP_RECEIVING;
        if (s->pub.save_to_disk) {
            if (s->pub.status_code >= 200 && s->pub.status_code < 300) {
                s->disk_file = open_downloads_file_for_write(s->pub.save_filename);
                if (!s->disk_file) {
                    http_fail_slot(s, "Cannot open \\DOWNLOADS file");
                    return;
                }
                sync_download_entry(s, "Downloading...", false);
            } else {
                sync_download_entry(s, "HTTP error", false);
            }
        } else if (s->pub.content_length > 0 && s->pub.content_length <= MAX_HTTP_BODY_MEM) {
            http_grow_body_buf(s, s->pub.content_length);
        }

        if (pos < len) {
            http_append_body(s, data + pos, len - pos);
        }
    } else {
        s->pub.state = HTTP_RECEIVING;
        http_append_body(s, data, len);
    }
}

static void http_tick_slot(HttpSlot* s) {
    if (!s->slot_used || !s->pub.active) return;
    if (s->pub.state == HTTP_DONE || s->pub.state == HTTP_ERROR || s->pub.state == HTTP_IDLE) return;

    uint64_t now = now_ms();
    uint64_t elapsed = now - s->phase_start_ms;

    if (s->tcp_phase == TCP_WAIT_DNS) {
        uint32_t ip = 0;
        if (dns_cache_lookup(s->pub.host, &ip)) {
            s->remote_ip = ip;
            s->pub.state = HTTP_CONNECTING;
            s->tcp_phase = TCP_WAIT_ARP;
            s->phase_start_ms = now;
            s->retries = 0;
            return;
        }
        if (elapsed >= 500) {
            if (s->retries < 4) {
                s->retries++;
                s->phase_start_ms = now;
                send_dns_a_query(s->pub.host, s->dns_txid, s->dns_sport);
            } else {
                http_fail_slot(s, "DNS resolution timed out");
            }
        }
    } else if (s->tcp_phase == TCP_WAIT_ARP) {
        uint8_t mac[6];
        uint32_t nh = ip_next_hop(s->remote_ip);
        if (nh == 0xFFFFFFFFU || arp_lookup(nh, mac)) {
            uint32_t isn = 0x13570000U ^ ((uint32_t)read_tsc() & 0xFFFFU) ^ ((uint32_t)s->pub.id << 16);
            s->snd_una = isn;
            s->snd_nxt = isn + 1;
            s->rcv_nxt = 0;
            s->tcp_phase = TCP_SYN_SENT;
            s->pub.state = HTTP_CONNECTING;
            s->phase_start_ms = now;
            s->retries = 0;
            tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, isn, 0, TCP_SYN, true, NULL, 0);
            return;
        }
        if (s->retries == 0) {
            arp_send_request(nh);
            s->retries = 1;
            s->phase_start_ms = now;
        } else if (elapsed >= 350) {
            if (s->retries < 4) {
                s->retries++;
                s->phase_start_ms = now;
                arp_send_request(nh);
            } else {
                http_fail_slot(s, "ARP gateway/host unreachable");
            }
        }
    } else if (s->tcp_phase == TCP_SYN_SENT) {
        if (elapsed >= 500) {
            if (s->retries < 4) {
                s->retries++;
                s->phase_start_ms = now;
                tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_una, 0, TCP_SYN, true, NULL, 0);
            } else {
                http_fail_slot(s, "TCP connection timed out");
            }
        }
    } else if (s->tcp_phase == TCP_ESTABLISHED) {
        if (s->pub.state == HTTP_SENDING && s->snd_una != s->snd_nxt && elapsed >= 600) {
            if (s->retries < 3) {
                s->retries++;
                http_send_get_request(s);
            } else {
                http_fail_slot(s, "HTTP request timed out");
            }
        } else if (now - s->last_activity_ms >= 8000) {
            if (s->headers_parsed && s->pub.bytes_received > 0 && s->pub.content_length < 0 && !s->is_chunked) {
                http_complete_slot(s);
            } else {
                http_fail_slot(s, "HTTP receive timed out");
            }
        }
    }
}

// ============================================================================
// Incoming Packet Handlers (ARP, IPv4, ICMP, UDP, TCP)
// ============================================================================

static void handle_arp_rx(const uint8_t* payload, int len) {
    if (len < (int)sizeof(ArpPkt)) return;
    const ArpPkt* arp = (const ArpPkt*)payload;
    if (ntohs(arp->htype) != 1 || ntohs(arp->ptype) != ETH_TYPE_IPV4) return;
    if (arp->hlen != 6 || arp->plen != 4) return;

    uint16_t oper = ntohs(arp->oper);
    uint32_t spa = ntohl(arp->spa);
    uint32_t tpa = ntohl(arp->tpa);

    arp_learn(spa, arp->sha);

    if (oper == 1 && g_status.ip_addr != 0 && tpa == g_status.ip_addr) {
        arp_send_packet(2, arp->sha, spa);
    }
}

static void handle_icmp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t* payload, int len) {
    if (len < (int)sizeof(IcmpHdr)) return;
    if (checksum_16(payload, len, 0) != 0) return;

    const IcmpHdr* icmp = (const IcmpHdr*)payload;
    if (icmp->type == 0 && icmp->code == 0) {
        if (g_ping_waiting &&
            src_ip == g_ping_target_ip &&
            ntohs(icmp->id) == g_ping_id &&
            ntohs(icmp->seq) == g_ping_seq) {
            g_ping_reply_ok = true;
        }
    } else if (icmp->type == 8 && icmp->code == 0 && g_status.ip_addr != 0 && dst_ip == g_status.ip_addr) {
        if (len <= 1480) {
            uint8_t reply[1480];
            n_memcpy(reply, payload, len);
            IcmpHdr* rh = (IcmpHdr*)reply;
            rh->type = 0;
            rh->code = 0;
            rh->checksum = 0;
            rh->checksum = checksum_16(reply, len, 0);
            ip_send(src_ip, IP_PROTO_ICMP, reply, len);
        }
    }
}

static void handle_udp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t* payload, int len) {
    if (len < (int)sizeof(UdpHdr)) return;
    const UdpHdr* udp = (const UdpHdr*)payload;
    uint16_t sport = ntohs(udp->src_port);
    uint16_t dport = ntohs(udp->dst_port);
    uint16_t ulen  = ntohs(udp->length);
    if (ulen < sizeof(UdpHdr) || (int)ulen > len) return;

    const uint8_t* body = payload + sizeof(UdpHdr);
    int body_len = (int)ulen - (int)sizeof(UdpHdr);

    if (dport == 68 && sport == 67) {
        dhcp_handle_rx(src_ip, body, body_len);
        return;
    }

    if (g_status.ip_addr != 0 && dst_ip != g_status.ip_addr && dst_ip != 0xFFFFFFFFU) {
        return;
    }

    if (sport == 53) {
        if (g_sync_dns_waiting && dport == g_sync_dns_sport) {
            uint32_t resolved = dns_parse_a_response(body, body_len, g_sync_dns_txid);
            if (resolved != 0) {
                g_sync_dns_result = resolved;
                g_sync_dns_done = true;
            }
        }
        for (int i = 0; i < MAX_HTTP_SLOTS; i++) {
            HttpSlot* s = &g_http_slots[i];
            if (s->slot_used && s->tcp_phase == TCP_WAIT_DNS && dport == s->dns_sport) {
                uint32_t resolved = dns_parse_a_response(body, body_len, s->dns_txid);
                if (resolved != 0) {
                    dns_cache_store(s->pub.host, resolved);
                    s->remote_ip = resolved;
                    s->pub.state = HTTP_CONNECTING;
                    s->tcp_phase = TCP_WAIT_ARP;
                    s->phase_start_ms = now_ms();
                    s->retries = 0;
                }
            }
        }
    }
}

static void handle_tcp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t* payload, int len) {
    if (len < (int)sizeof(TcpHdr)) return;
    if (g_status.ip_addr == 0 || dst_ip != g_status.ip_addr) return;

    const TcpHdr* tcp = (const TcpHdr*)payload;
    int hdr_len = ((tcp->data_off >> 4) & 0x0FU) * 4;
    if (hdr_len < 20 || hdr_len > len) return;

    uint16_t sport = ntohs(tcp->src_port);
    uint16_t dport = ntohs(tcp->dst_port);
    uint32_t seq   = ntohl(tcp->seq);
    uint32_t ack   = ntohl(tcp->ack);
    uint8_t  flags = tcp->flags;

    const uint8_t* data = payload + hdr_len;
    int data_len = len - hdr_len;

    HttpSlot* s = NULL;
    for (int i = 0; i < MAX_HTTP_SLOTS; i++) {
        if (g_http_slots[i].slot_used &&
            g_http_slots[i].tcp_phase != TCP_CLOSED &&
            g_http_slots[i].remote_ip == src_ip &&
            g_http_slots[i].pub.port == sport &&
            g_http_slots[i].local_port == dport) {
            s = &g_http_slots[i];
            break;
        }
    }
    if (!s) return;

    s->last_activity_ms = now_ms();

    if (flags & TCP_RST) {
        if (s->headers_parsed && s->pub.bytes_received > 0 &&
            (s->pub.content_length < 0 || s->pub.bytes_received >= s->pub.content_length)) {
            http_complete_slot(s);
        } else {
            http_fail_slot(s, "Connection reset by peer");
        }
        return;
    }

    if (s->tcp_phase == TCP_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == s->snd_nxt) {
            s->snd_una = ack;
            s->rcv_nxt = seq + 1;
            s->tcp_phase = TCP_ESTABLISHED;
            s->retries = 0;
            tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_ACK, false, NULL, 0);
            http_send_get_request(s);
        }
        return;
    }

    if (s->tcp_phase != TCP_ESTABLISHED) return;

    if ((flags & TCP_ACK) && SEQ_GT(ack, s->snd_una) && SEQ_LEQ(ack, s->snd_nxt)) {
        s->snd_una = ack;
    }

    if (data_len > 0) {
        if (seq == s->rcv_nxt) {
            s->rcv_nxt += (uint32_t)data_len;
            tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_ACK, false, NULL, 0);
            http_process_rx_bytes(s, data, data_len);
        } else if (SEQ_LT(seq, s->rcv_nxt)) {
            uint32_t overlap = s->rcv_nxt - seq;
            if (overlap < (uint32_t)data_len) {
                int new_len = data_len - (int)overlap;
                s->rcv_nxt += (uint32_t)new_len;
                tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_ACK, false, NULL, 0);
                http_process_rx_bytes(s, data + overlap, new_len);
            } else {
                tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_ACK, false, NULL, 0);
            }
        } else {
            // Out-of-order segment: duplicate ACK so peer fast-retransmits
            tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_ACK, false, NULL, 0);
            return;
        }
    }

    if ((flags & TCP_FIN) && (s->pub.state != HTTP_DONE && s->pub.state != HTTP_ERROR)) {
        if (seq + (uint32_t)data_len == s->rcv_nxt) {
            s->rcv_nxt += 1;
            s->peer_fin = true;
            if (!s->fin_sent) {
                tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_FIN | TCP_ACK, false, NULL, 0);
                s->snd_nxt++;
                s->fin_sent = true;
            } else {
                tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_ACK, false, NULL, 0);
            }
            if (s->headers_parsed) {
                http_complete_slot(s);
            } else {
                http_fail_slot(s, "Peer closed connection before HTTP headers");
            }
        }
    }
}

static void handle_ipv4_rx(const uint8_t* payload, int len) {
    if (len < (int)sizeof(Ipv4Hdr)) return;
    const Ipv4Hdr* ip = (const Ipv4Hdr*)payload;
    if (((ip->ver_ihl >> 4) & 0x0FU) != 4) return;
    int ihl = (ip->ver_ihl & 0x0FU) * 4;
    if (ihl < 20 || ihl > len) return;

    uint16_t tot_len = ntohs(ip->tot_len);
    if (tot_len < ihl || (int)tot_len > len) return;

    uint16_t frag = ntohs(ip->frag_off);
    if ((frag & 0x1FFFU) != 0 || (frag & 0x2000U) != 0) return;

    if (checksum_16(payload, ihl, 0) != 0) return;

    uint32_t src_ip = ntohl(ip->src_ip);
    uint32_t dst_ip = ntohl(ip->dst_ip);
    const uint8_t* l4 = payload + ihl;
    int l4_len = (int)tot_len - ihl;

    switch (ip->proto) {
        case IP_PROTO_ICMP: handle_icmp_rx(src_ip, dst_ip, l4, l4_len); break;
        case IP_PROTO_UDP:  handle_udp_rx(src_ip, dst_ip, l4, l4_len); break;
        case IP_PROTO_TCP:  handle_tcp_rx(src_ip, dst_ip, l4, l4_len); break;
        default: break;
    }
}

static void handle_eth_frame(const uint8_t* frame, int len) {
    if (len < (int)sizeof(EthHdr)) return;
    const EthHdr* eth = (const EthHdr*)frame;
    uint16_t etype = ntohs(eth->ethertype);
    const uint8_t* payload = frame + sizeof(EthHdr);
    int plen = len - (int)sizeof(EthHdr);

    if (etype == ETH_TYPE_ARP) {
        handle_arp_rx(payload, plen);
    } else if (etype == ETH_TYPE_IPV4) {
        handle_ipv4_rx(payload, plen);
    }
}

// ============================================================================
// Public Exported C API (Matching net.h)
// ============================================================================

extern "C" {

void net_format_ip(uint32_t ip, char* out, int max_len) {
    if (!out || max_len <= 0) return;
    uint8_t a = (uint8_t)((ip >> 24) & 0xFFU);
    uint8_t b = (uint8_t)((ip >> 16) & 0xFFU);
    uint8_t c = (uint8_t)((ip >> 8)  & 0xFFU);
    uint8_t d = (uint8_t)(ip & 0xFFU);

    int pos = 0;
    uint8_t octs[4] = {a, b, c, d};
    for (int i = 0; i < 4; i++) {
        char num[8];
        int n = int_to_ascii((int)octs[i], num, 8);
        for (int j = 0; j < n && pos < max_len - 1; j++) {
            out[pos++] = num[j];
        }
        if (i < 3 && pos < max_len - 1) {
            out[pos++] = '.';
        }
    }
    out[pos] = '\0';
}

void net_init(EFI_SYSTEM_TABLE* st) {
    if (st) g_st = st;

    n_memset(&g_status, 0, sizeof(g_status));
    n_strcpy(g_status.adapter_name, "No Network Adapter", 64);
    n_strcpy(g_status.mac_str, "00:00:00:00:00:00", 24);
    n_strcpy(g_status.ip_str, "0.0.0.0", 20);
    n_strcpy(g_status.gw_str, "0.0.0.0", 20);
    n_strcpy(g_status.dns_str, "0.0.0.0", 20);

    n_memset(g_arp_cache, 0, sizeof(g_arp_cache));
    n_memset(g_dns_cache, 0, sizeof(g_dns_cache));
    n_memset(g_http_slots, 0, sizeof(g_http_slots));
    n_memset(g_downloads, 0, sizeof(g_downloads));
    g_download_count = 0;
    g_snp = NULL;

    // Calibrate TSC for non-blocking millisecond timing
    g_tsc_boot = read_tsc();
    if (g_st && g_st->BootServices) {
        uint64_t t0 = read_tsc();
        g_st->BootServices->Stall(10000); // 10 ms
        uint64_t t1 = read_tsc();
        uint64_t diff = (t1 > t0) ? (t1 - t0) : 0;
        g_tsc_per_ms = (diff >= 10000ULL) ? (diff / 10ULL) : 1000000ULL;
    }

    scan_existing_downloads();

    char pci_name[64];
    probe_pci_nic_name(pci_name, 64);

    if (!g_st || !g_st->BootServices) {
        g_initialized = true;
        return;
    }

    UINTN handle_count = 0;
    EFI_HANDLE* handles = NULL;
    EFI_STATUS lstatus = g_st->BootServices->LocateHandleBuffer(
        ByProtocol,
        &g_snp_guid,
        NULL,
        &handle_count,
        &handles
    );

    EFI_SIMPLE_NETWORK_PROTOCOL* fallback_snp = NULL;
    if (!EFI_ERROR(lstatus) && handles && handle_count > 0) {
        for (UINTN i = 0; i < handle_count; i++) {
            EFI_SIMPLE_NETWORK_PROTOCOL* snp = NULL;
            if (EFI_ERROR(g_st->BootServices->HandleProtocol(handles[i], &g_snp_guid, (VOID**)&snp)) || !snp || !snp->Mode) {
                continue;
            }

            if (snp->Mode->State == EfiSimpleNetworkStopped) {
                if (EFI_ERROR(snp->Start(snp))) continue;
            }
            if (snp->Mode->State == EfiSimpleNetworkStarted) {
                if (EFI_ERROR(snp->Initialize(snp, 0, 0))) continue;
            }
            if (snp->Mode->State != EfiSimpleNetworkInitialized) {
                continue;
            }

            UINT32 desired = EFI_SIMPLE_NETWORK_RECEIVE_UNICAST | EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST;
            UINT32 mask = snp->Mode->ReceiveFilterMask ? (desired & snp->Mode->ReceiveFilterMask) : desired;
            if (mask != 0) {
                snp->ReceiveFilters(snp, mask, 0, FALSE, 0, NULL);
            }

            if (snp->Mode->HwAddressSize != 6) continue;
            if (!fallback_snp) fallback_snp = snp;
            if (snp->Mode->MediaPresent) {
                g_snp = snp;
                break;
            }
        }
        if (!g_snp) g_snp = fallback_snp;
        g_st->BootServices->FreePool(handles);
    }

    if (g_snp && g_snp->Mode) {
        g_status.nic_present = true;
        n_memcpy(g_status.mac, g_snp->Mode->CurrentAddress.Addr, 6);
        format_mac_str(g_status.mac, g_status.mac_str, 24);
        if (pci_name[0] != '\0') {
            n_strcpy(g_status.adapter_name, pci_name, 64);
        } else {
            n_strcpy(g_status.adapter_name, "UEFI Simple Network (SNP)", 64);
        }

        // Brief link-up settle check (up to 100ms) if NIC just initialized
        for (int w = 0; w < 10 && !g_snp->Mode->MediaPresent; w++) {
            stall_us(10000);
        }
        g_status.link_up = (g_snp->Mode->MediaPresent != FALSE);
        g_initialized = true;

        if (g_status.link_up) {
            net_dhcp_renew();
        }
    } else {
        if (pci_name[0] != '\0') {
            n_strcpy(g_status.adapter_name, pci_name, 64);
        }
        g_initialized = true;
    }
}

void net_tick(void) {
    if (!g_initialized || !g_snp || !g_snp->Mode) return;

    g_status.link_up = (g_snp->Mode->MediaPresent != FALSE);

    UINT32 intr = 0;
    VOID* tx_done = NULL;
    g_snp->GetStatus(g_snp, &intr, &tx_done);

    for (int i = 0; i < RX_FRAMES_PER_TICK; i++) {
        UINTN hdr_sz = 0;
        UINTN buf_sz = sizeof(g_rx_frame);
        EFI_STATUS st = g_snp->Receive(g_snp, &hdr_sz, &buf_sz, (VOID*)g_rx_frame, NULL, NULL, NULL);
        if (st == EFI_NOT_READY || EFI_ERROR(st)) {
            break;
        }
        if (buf_sz < sizeof(EthHdr) || buf_sz > sizeof(g_rx_frame)) {
            continue;
        }
        g_status.rx_packets++;
        g_status.rx_bytes += (uint64_t)buf_sz;
        handle_eth_frame(g_rx_frame, (int)buf_sz);
    }

    for (int i = 0; i < MAX_HTTP_SLOTS; i++) {
        if (g_http_slots[i].slot_used) {
            http_tick_slot(&g_http_slots[i]);
        }
    }
}

bool net_dhcp_renew(void) {
    if (!g_initialized || !g_snp || !g_status.nic_present) return false;
    g_status.link_up = (g_snp->Mode && g_snp->Mode->MediaPresent != FALSE);
    if (!g_status.link_up) return false;

    g_dhcp_xid = 0x424C0000U ^ ((uint32_t)read_tsc() & 0xFFFFU) ^ (++g_dhcp_seq);
    g_dhcp_offer_ok = false;
    g_dhcp_ack_ok = false;
    g_dhcp_offered_ip = 0;
    g_dhcp_server_id = 0;
    g_dhcp_subnet = 0;
    g_dhcp_gw = 0;
    g_dhcp_dns = 0;

    uint32_t saved_ip = g_status.ip_addr;
    g_status.ip_addr = 0;

    // Step 1: DISCOVER -> OFFER (max ~750ms)
    dhcp_send_msg(1, 0, 0);
    for (int step = 0; step < 75 && !g_dhcp_offer_ok; step++) {
        net_tick();
        if (g_dhcp_offer_ok) break;
        stall_us(10000);
        if (step == 35) {
            dhcp_send_msg(1, 0, 0);
        }
    }

    if (!g_dhcp_offer_ok) {
        g_status.ip_addr = saved_ip;
        return false;
    }

    // Step 2: REQUEST -> ACK (max ~750ms)
    dhcp_send_msg(3, g_dhcp_offered_ip, g_dhcp_server_id);
    for (int step = 0; step < 75 && !g_dhcp_ack_ok; step++) {
        net_tick();
        if (g_dhcp_ack_ok) break;
        stall_us(10000);
        if (step == 35) {
            dhcp_send_msg(3, g_dhcp_offered_ip, g_dhcp_server_id);
        }
    }

    if (!g_dhcp_ack_ok) {
        g_status.ip_addr = saved_ip;
        return false;
    }

    g_status.ip_addr = g_dhcp_offered_ip;
    g_status.subnet_mask = g_dhcp_subnet ? g_dhcp_subnet : ip_pack(255, 255, 255, 0);
    g_status.gateway_ip = g_dhcp_gw;
    g_status.dns_ip = g_dhcp_dns ? g_dhcp_dns : ip_pack(8, 8, 8, 8);
    g_status.dhcp_bound = true;

    net_format_ip(g_status.ip_addr, g_status.ip_str, 20);
    net_format_ip(g_status.gateway_ip, g_status.gw_str, 20);
    net_format_ip(g_status.dns_ip, g_status.dns_str, 20);

    // Gratuitous ARP & pre-warm gateway ARP cache
    arp_send_request(g_status.ip_addr);
    if (g_status.gateway_ip != 0) {
        arp_send_request(g_status.gateway_ip);
    }
    return true;
}

void net_get_status(NetStatus* out) {
    if (!out) return;
    if (g_snp && g_snp->Mode) {
        g_status.link_up = (g_snp->Mode->MediaPresent != FALSE);
    }
    net_format_ip(g_status.ip_addr, g_status.ip_str, 20);
    net_format_ip(g_status.gateway_ip, g_status.gw_str, 20);
    net_format_ip(g_status.dns_ip, g_status.dns_str, 20);
    n_memcpy(out, &g_status, sizeof(NetStatus));
}

uint32_t net_dns_resolve(const char* host, int timeout_ms) {
    if (!host || !host[0]) return 0;

    uint32_t literal = 0;
    if (parse_ipv4_literal(host, &literal)) {
        return literal;
    }

    uint32_t cached = 0;
    if (dns_cache_lookup(host, &cached)) {
        return cached;
    }

    if (!g_initialized || !g_snp || !g_status.nic_present || !g_status.link_up || g_status.ip_addr == 0) {
        return 0;
    }

    int tmo = (timeout_ms > 0) ? timeout_ms : 2000;
    uint32_t dns_srv = g_status.dns_ip ? g_status.dns_ip : ip_pack(8, 8, 8, 8);
    uint8_t mac[6];
    if (!arp_resolve_blocking(dns_srv, mac, (tmo < 600) ? tmo : 600)) {
        return 0;
    }

    g_sync_dns_txid = (uint16_t)(0x4200U + ((uint16_t)read_tsc() & 0xFFU));
    g_sync_dns_sport = g_ephemeral_port++;
    if (g_ephemeral_port < 49152) g_ephemeral_port = 49152;
    g_sync_dns_result = 0;
    g_sync_dns_done = false;
    g_sync_dns_waiting = true;

    send_dns_a_query(host, g_sync_dns_txid, g_sync_dns_sport);

    uint64_t start = now_ms();
    bool retried = false;
    int max_iters = (tmo / 2) + 50;
    for (int it = 0; it < max_iters && (int)(now_ms() - start) < tmo; it++) {
        net_tick();
        if (g_sync_dns_done) break;
        stall_us(2000);
        if (!retried && (int)(now_ms() - start) >= tmo / 2) {
            retried = true;
            send_dns_a_query(host, g_sync_dns_txid, g_sync_dns_sport);
        }
    }

    g_sync_dns_waiting = false;
    if (g_sync_dns_done && g_sync_dns_result != 0) {
        dns_cache_store(host, g_sync_dns_result);
        return g_sync_dns_result;
    }
    return 0;
}

int net_ping(const char* target, int timeout_ms) {
    if (!target || !target[0]) return -1;
    if (!g_initialized || !g_snp || !g_status.nic_present || !g_status.link_up || g_status.ip_addr == 0) {
        return -1;
    }

    int tmo = (timeout_ms > 0) ? timeout_ms : 2000;
    uint32_t dst_ip = net_dns_resolve(target, tmo > 1000 ? tmo / 2 : tmo);
    if (dst_ip == 0) return -1;

    uint8_t mac[6];
    if (!arp_resolve_blocking(dst_ip, mac, tmo > 500 ? 500 : tmo)) {
        return -1;
    }

    uint8_t pkt[40];
    IcmpHdr* icmp = (IcmpHdr*)pkt;
    icmp->type = 8; // Echo Request
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(g_ping_id);
    icmp->seq = htons(++g_ping_seq);
    static const char tag[32] = "BlankOS-ICMP-Ping-Probe-0000000";
    n_memcpy(pkt + 8, tag, 32);
    icmp->checksum = checksum_16(pkt, 40, 0);

    g_ping_target_ip = dst_ip;
    g_ping_reply_ok = false;
    g_ping_waiting = true;

    uint64_t start = now_ms();
    if (!ip_send(dst_ip, IP_PROTO_ICMP, pkt, 40)) {
        g_ping_waiting = false;
        return -1;
    }

    int max_iters = tmo + 50;
    for (int it = 0; it < max_iters && (int)(now_ms() - start) < tmo; it++) {
        net_tick();
        if (g_ping_reply_ok) {
            g_ping_waiting = false;
            int rtt = (int)(now_ms() - start);
            return (rtt >= 0) ? rtt : 0;
        }
        stall_us(1000);
    }

    g_ping_waiting = false;
    return -1;
}

int net_http_start(const char* url, const char* save_filename) {
    if (!url || !url[0]) return -1;

    int idx = -1;
    for (int i = 0; i < MAX_HTTP_SLOTS; i++) {
        if (!g_http_slots[i].slot_used) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;

    HttpSlot* s = &g_http_slots[idx];
    n_memset(s, 0, sizeof(HttpSlot));
    s->slot_used = true;
    s->pub.id = idx;
    s->pub.active = true;
    s->pub.content_length = -1;
    s->dl_entry_idx = -1;
    n_strcpy(s->pub.url, url, 512);

    bool is_https = false;
    if (!parse_http_url(url, &is_https, s->pub.host, 128, &s->pub.port, s->pub.path, 384)) {
        http_fail_slot(s, "Malformed URL");
        return idx;
    }
    s->pub.is_https = is_https;

    if (save_filename && save_filename[0]) {
        s->pub.save_to_disk = true;
        // Strip any directory slashes from save_filename
        const char* base = save_filename;
        for (int k = 0; save_filename[k]; k++) {
            if (save_filename[k] == '/' || save_filename[k] == '\\') {
                base = &save_filename[k + 1];
            }
        }
        n_strcpy(s->pub.save_filename, base[0] ? base : "download.bin", 128);

        if (g_download_count < MAX_DOWNLOADS) {
            s->dl_entry_idx = g_download_count++;
            DownloadEntry* de = &g_downloads[s->dl_entry_idx];
            n_memset(de, 0, sizeof(DownloadEntry));
            n_strcpy(de->filename, s->pub.save_filename, 128);
            n_strcpy(de->url, s->pub.url, 512);
            n_strcpy(de->disk_path, "\\DOWNLOADS\\", 192);
            int dp = n_strlen(de->disk_path);
            n_strcpy(de->disk_path + dp, s->pub.save_filename, 192 - dp);
            de->state = HTTP_IDLE;
            n_strcpy(de->status_text, "Queued", 64);
        }
    }

    if (is_https) {
        http_fail_slot(s, "HTTPS requires TLS; use HTTP mirror");
        return idx;
    }

    if (!g_initialized || !g_snp || !g_status.nic_present || !g_status.link_up || g_status.ip_addr == 0) {
        http_fail_slot(s, "No network link or DHCP IP");
        return idx;
    }

    s->local_port = g_ephemeral_port++;
    if (g_ephemeral_port < 49152) g_ephemeral_port = 49152;
    s->phase_start_ms = now_ms();
    s->last_activity_ms = s->phase_start_ms;
    s->transfer_start_ms = s->phase_start_ms;

    uint32_t ip = 0;
    if (parse_ipv4_literal(s->pub.host, &ip) || dns_cache_lookup(s->pub.host, &ip)) {
        s->remote_ip = ip;
        s->pub.state = HTTP_CONNECTING;
        s->tcp_phase = TCP_WAIT_ARP;
        sync_download_entry(s, "Connecting...", false);
    } else {
        s->pub.state = HTTP_RESOLVING_DNS;
        s->tcp_phase = TCP_WAIT_DNS;
        s->dns_txid = (uint16_t)(0x4B00U + idx * 37U + (uint16_t)(now_ms() & 0xFFU));
        s->dns_sport = g_ephemeral_port++;
        if (g_ephemeral_port < 49152) g_ephemeral_port = 49152;
        send_dns_a_query(s->pub.host, s->dns_txid, s->dns_sport);
        sync_download_entry(s, "Resolving DNS...", false);
    }

    return idx;
}

const HttpTransfer* net_http_get(int transfer_id) {
    if (transfer_id < 0 || transfer_id >= MAX_HTTP_SLOTS) return NULL;
    if (!g_http_slots[transfer_id].slot_used) return NULL;
    return &g_http_slots[transfer_id].pub;
}

void net_http_close(int transfer_id) {
    if (transfer_id < 0 || transfer_id >= MAX_HTTP_SLOTS) return;
    HttpSlot* s = &g_http_slots[transfer_id];
    if (!s->slot_used) return;

    if (s->tcp_phase == TCP_ESTABLISHED && !s->fin_sent && s->remote_ip != 0) {
        tcp_send_segment(s->remote_ip, s->local_port, s->pub.port, s->snd_nxt, s->rcv_nxt, TCP_FIN | TCP_ACK, false, NULL, 0);
        s->fin_sent = true;
    }

    close_slot_disk_file(s);
    if (s->pub.body_buf) {
        kfree(s->pub.body_buf);
        s->pub.body_buf = NULL;
    }
    n_memset(s, 0, sizeof(HttpSlot));
    s->dl_entry_idx = -1;
}

int net_download_count(void) {
    return g_download_count;
}

const DownloadEntry* net_get_download(int idx) {
    if (idx < 0 || idx >= g_download_count) return NULL;
    return &g_downloads[idx];
}

void net_clear_finished_downloads(void) {
    int write_idx = 0;
    for (int i = 0; i < g_download_count; i++) {
        bool finished = (g_downloads[i].state == HTTP_DONE || g_downloads[i].state == HTTP_ERROR);
        if (!finished) {
            if (write_idx != i) {
                n_memcpy(&g_downloads[write_idx], &g_downloads[i], sizeof(DownloadEntry));
                for (int s = 0; s < MAX_HTTP_SLOTS; s++) {
                    if (g_http_slots[s].slot_used && g_http_slots[s].dl_entry_idx == i) {
                        g_http_slots[s].dl_entry_idx = write_idx;
                    }
                }
            }
            write_idx++;
        } else {
            for (int s = 0; s < MAX_HTTP_SLOTS; s++) {
                if (g_http_slots[s].slot_used && g_http_slots[s].dl_entry_idx == i) {
                    g_http_slots[s].dl_entry_idx = -1;
                }
            }
        }
    }
    g_download_count = write_idx;
}

} // extern "C"
