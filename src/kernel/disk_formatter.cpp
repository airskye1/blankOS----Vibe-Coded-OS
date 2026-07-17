#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <efi.h>

extern "C" {
    extern void dui_memset32(uint32_t* dst, uint32_t val, int count);
    
    static void memset8(uint8_t* dst, uint8_t val, int count) {
        for (int i = 0; i < count; i++) dst[i] = val;
    }

    // --- MBR & GPT Structures ---
    #pragma pack(push, 1)
    
    typedef struct {
        uint8_t  boot_indicator;
        uint8_t  start_head;
        uint8_t  start_sector;
        uint8_t  start_track;
        uint8_t  os_type;
        uint8_t  end_head;
        uint8_t  end_sector;
        uint8_t  end_track;
        uint32_t starting_lba;
        uint32_t size_in_lba;
    } MbrPartitionRecord;

    typedef struct {
        uint8_t  boot_code[440];
        uint32_t unique_mbr_signature;
        uint16_t unknown;
        MbrPartitionRecord partitions[4];
        uint16_t signature; // 0xAA55
    } MasterBootRecord;

    typedef struct {
        uint64_t signature; // "EFI PART" = 0x5452415020494645
        uint32_t revision;  // 0x00010000
        uint32_t header_size; // 92
        uint32_t header_crc32;
        uint32_t reserved;
        uint64_t my_lba;
        uint64_t alternate_lba;
        uint64_t first_usable_lba;
        uint64_t last_usable_lba;
        EFI_GUID disk_guid;
        uint64_t partition_entry_lba;
        uint32_t num_partition_entries;
        uint32_t size_of_partition_entry; // 128
        uint32_t partition_entry_array_crc32;
    } GptHeader;

    typedef struct {
        EFI_GUID partition_type_guid;
        EFI_GUID unique_partition_guid;
        uint64_t starting_lba;
        uint64_t ending_lba;
        uint64_t attributes;
        CHAR16   partition_name[36];
    } GptPartitionEntry;

    // --- FAT32 Structures ---
    typedef struct {
        uint8_t  jmp_boot[3];
        char     oem_name[8];
        uint16_t bytes_per_sector;
        uint8_t  sectors_per_cluster;
        uint16_t reserved_sector_count;
        uint8_t  num_fats;
        uint16_t root_entry_count; // 0 for FAT32
        uint16_t total_sectors_16; // 0 for FAT32
        uint8_t  media;
        uint16_t fat_size_16;      // 0 for FAT32
        uint16_t sectors_per_track;
        uint16_t number_of_heads;
        uint32_t hidden_sectors;
        uint32_t total_sectors_32;
        // FAT32 specific
        uint32_t fat_size_32;
        uint16_t ext_flags;
        uint16_t fs_version;
        uint32_t root_cluster;
        uint16_t fs_info;
        uint16_t backup_boot_sector;
        uint8_t  reserved[12];
        uint8_t  drive_number;
        uint8_t  reserved1;
        uint8_t  boot_signature;
        uint32_t volume_id;
        char     volume_label[11];
        char     fs_type[8];
        uint8_t  boot_code[420];
        uint16_t boot_sector_signature; // 0xAA55
    } Fat32BootSector;
    
    typedef struct {
        uint32_t lead_sig; // 0x41615252
        uint8_t  reserved1[480];
        uint32_t struc_sig; // 0x61417272
        uint32_t free_count;
        uint32_t nxt_free;
        uint8_t  reserved2[12];
        uint32_t trail_sig; // 0xAA550000
    } Fat32FsInfo;

    #pragma pack(pop)

    // A simple CRC32 implementation
    static uint32_t calculate_crc32(const uint8_t *data, size_t length) {
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; i++) {
            crc ^= data[i];
            for (int j = 0; j < 8; j++) {
                crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
            }
        }
        return ~crc;
    }

    // Write a block buffer to disk
    static bool write_blocks(EFI_BLOCK_IO_PROTOCOL* BlockIo, uint64_t lba, uint32_t size, void* buffer) {
        EFI_STATUS status = BlockIo->WriteBlocks(BlockIo, BlockIo->Media->MediaId, lba, size, buffer);
        return (status == EFI_SUCCESS);
    }

    bool format_disk_gpt_fat32(EFI_SYSTEM_TABLE *SystemTable, EFI_BLOCK_IO_PROTOCOL* BlockIo) {
        if (!BlockIo || !BlockIo->Media || BlockIo->Media->ReadOnly) return false;
        
        uint32_t block_size = BlockIo->Media->BlockSize;
        if (block_size != 512) return false; // For simplicity, only support 512 byte sectors

        uint64_t last_lba = BlockIo->Media->LastBlock;
        if (last_lba < 100000) return false; // Disk too small (needs > 50MB)

        uint8_t buffer[512]; // We'll reuse this 512-byte buffer for sector writes

        // ----------------------------------------------------
        // 1. Protective MBR (LBA 0)
        // ----------------------------------------------------
        memset8(buffer, 0, 512);
        MasterBootRecord* mbr = (MasterBootRecord*)buffer;
        mbr->partitions[0].boot_indicator = 0;
        mbr->partitions[0].start_head = 0;
        mbr->partitions[0].start_sector = 2;
        mbr->partitions[0].start_track = 0;
        mbr->partitions[0].os_type = 0xEE; // GPT Protective
        mbr->partitions[0].end_head = 0xFF;
        mbr->partitions[0].end_sector = 0xFF;
        mbr->partitions[0].end_track = 0xFF;
        mbr->partitions[0].starting_lba = 1;
        mbr->partitions[0].size_in_lba = (last_lba > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)last_lba;
        mbr->signature = 0xAA55;
        if (!write_blocks(BlockIo, 0, 512, buffer)) return false;

        // ----------------------------------------------------
        // 2. GPT Partition Entries (LBA 2..33)
        // ----------------------------------------------------
        // We want a single partition spanning the whole usable space.
        uint64_t first_usable_lba = 34;
        uint64_t last_usable_lba = last_lba - 33;
        uint64_t partition_start = 2048; // Align to 1MB
        if (partition_start < first_usable_lba) partition_start = first_usable_lba;
        uint64_t partition_end = last_usable_lba;
        uint64_t partition_size_lba = partition_end - partition_start + 1;

        // EFI System Partition GUID: C12A7328-F81F-11D2-BA4B-00A0C93EC93B
        EFI_GUID esp_guid = {0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
        EFI_GUID unique_guid = {0x12345678, 0x1234, 0x1234, {0x12, 0x34, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}}; // Static for now

        // Create the 128-byte partition entry array (we need 128 entries = 32 sectors of 512 bytes)
        // Let's just build it in a 16KB buffer
        uint8_t part_array[16384];
        memset8(part_array, 0, 16384);
        GptPartitionEntry* entry = (GptPartitionEntry*)part_array;
        entry->partition_type_guid = esp_guid;
        entry->unique_partition_guid = unique_guid;
        entry->starting_lba = partition_start;
        entry->ending_lba = partition_end;
        entry->attributes = 0;
        
        // Name: "EFI System"
        const CHAR16* name = L"EFI System";
        for (int i=0; name[i]; i++) entry->partition_name[i] = name[i];

        uint32_t part_array_crc = calculate_crc32(part_array, 16384);

        if (!write_blocks(BlockIo, 2, 16384, part_array)) return false;

        // ----------------------------------------------------
        // 3. GPT Header (LBA 1)
        // ----------------------------------------------------
        memset8(buffer, 0, 512);
        GptHeader* gpt = (GptHeader*)buffer;
        gpt->signature = 0x5452415020494645ULL; // "EFI PART"
        gpt->revision = 0x00010000;
        gpt->header_size = 92;
        gpt->my_lba = 1;
        gpt->alternate_lba = last_lba;
        gpt->first_usable_lba = first_usable_lba;
        gpt->last_usable_lba = last_usable_lba;
        
        // Disk GUID
        EFI_GUID disk_guid = {0x87654321, 0x4321, 0x4321, {0x21, 0x43, 0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB}};
        gpt->disk_guid = disk_guid;
        
        gpt->partition_entry_lba = 2;
        gpt->num_partition_entries = 128;
        gpt->size_of_partition_entry = 128;
        gpt->partition_entry_array_crc32 = part_array_crc;

        // Calculate Header CRC
        gpt->header_crc32 = 0;
        gpt->header_crc32 = calculate_crc32((uint8_t*)gpt, 92);
        
        if (!write_blocks(BlockIo, 1, 512, buffer)) return false;

        // (We skip writing the backup GPT at the end of the disk for simplicity in this demo)

        // ----------------------------------------------------
        // 4. FAT32 Formatting (Starts at partition_start)
        // ----------------------------------------------------
        memset8(buffer, 0, 512);
        Fat32BootSector* vbr = (Fat32BootSector*)buffer;
        
        // JMP short 0x58, NOP
        vbr->jmp_boot[0] = 0xEB; vbr->jmp_boot[1] = 0x58; vbr->jmp_boot[2] = 0x90;
        
        const char* oem = "MSWIN4.1";
        for (int i=0; i<8; i++) vbr->oem_name[i] = oem[i];
        
        vbr->bytes_per_sector = 512;
        vbr->sectors_per_cluster = 8; // 4KB clusters
        vbr->reserved_sector_count = 32;
        vbr->num_fats = 2;
        vbr->media = 0xF8;
        vbr->hidden_sectors = partition_start;
        vbr->total_sectors_32 = partition_size_lba;
        
        uint32_t usable_sectors = partition_size_lba - vbr->reserved_sector_count;
        // Approximation of FAT size:
        uint32_t fat_size = (usable_sectors / vbr->sectors_per_cluster) * 4 / 512 + 1;
        vbr->fat_size_32 = fat_size;
        
        vbr->root_cluster = 2;
        vbr->fs_info = 1;
        vbr->backup_boot_sector = 6;
        vbr->drive_number = 0x80;
        vbr->boot_signature = 0x29;
        vbr->volume_id = 0x12345678;
        
        const char* vol_label = "BLANKOS    ";
        for(int i=0; i<11; i++) vbr->volume_label[i] = vol_label[i];
        
        const char* fs_type = "FAT32   ";
        for(int i=0; i<8; i++) vbr->fs_type[i] = fs_type[i];
        
        vbr->boot_sector_signature = 0xAA55;
        
        // Write Main VBR
        if (!write_blocks(BlockIo, partition_start + 0, 512, buffer)) return false;
        // Write Backup VBR
        if (!write_blocks(BlockIo, partition_start + 6, 512, buffer)) return false;

        // ----------------------------------------------------
        // 5. FAT32 FSInfo Sector
        // ----------------------------------------------------
        memset8(buffer, 0, 512);
        Fat32FsInfo* fsinfo = (Fat32FsInfo*)buffer;
        fsinfo->lead_sig = 0x41615252;
        fsinfo->struc_sig = 0x61417272;
        fsinfo->free_count = 0xFFFFFFFF; // Unknown
        fsinfo->nxt_free = 0xFFFFFFFF; // Unknown
        fsinfo->trail_sig = 0xAA550000;
        
        if (!write_blocks(BlockIo, partition_start + 1, 512, buffer)) return false;
        if (!write_blocks(BlockIo, partition_start + 7, 512, buffer)) return false;

        // ----------------------------------------------------
        // 6. FAT Tables
        // ----------------------------------------------------
        // We only write the first sector of the FATs to initialize clusters 0, 1, and 2
        memset8(buffer, 0, 512);
        uint32_t* fat_entries = (uint32_t*)buffer;
        fat_entries[0] = 0x0FFFFFF8; // Cluster 0: Media type
        fat_entries[1] = 0x0FFFFFFF; // Cluster 1: EOF (reserved)
        fat_entries[2] = 0x0FFFFFFF; // Cluster 2: Root Directory (EOF for now)

        uint64_t fat1_start = partition_start + vbr->reserved_sector_count;
        uint64_t fat2_start = fat1_start + fat_size;
        
        // Zero out the FAT sectors (only the first few to be safe and fast)
        uint8_t zeros[512];
        memset8(zeros, 0, 512);
        for(uint32_t i = 1; i < 32; i++) {
            write_blocks(BlockIo, fat1_start + i, 512, zeros);
            write_blocks(BlockIo, fat2_start + i, 512, zeros);
        }

        // Write initialized FAT start
        if (!write_blocks(BlockIo, fat1_start, 512, buffer)) return false;
        if (!write_blocks(BlockIo, fat2_start, 512, buffer)) return false;

        // ----------------------------------------------------
        // 7. Root Directory (Cluster 2)
        // ----------------------------------------------------
        uint64_t data_start = fat2_start + fat_size;
        // Zero out the first cluster of the root directory
        for (int i = 0; i < vbr->sectors_per_cluster; i++) {
            if (!write_blocks(BlockIo, data_start + i, 512, zeros)) return false;
        }

        return true;
    }
}
