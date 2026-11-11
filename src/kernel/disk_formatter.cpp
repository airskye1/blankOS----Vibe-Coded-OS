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

    static bool write_blocks(EFI_BLOCK_IO_PROTOCOL* BlockIo, uint64_t lba, uint32_t size, void* buffer) {
        if (!BlockIo || !BlockIo->Media || !BlockIo->WriteBlocks) return false;
        EFI_STATUS status = BlockIo->WriteBlocks(BlockIo, BlockIo->Media->MediaId, lba, size, buffer);
        if (status == EFI_MEDIA_CHANGED) {
            if (BlockIo->Reset) BlockIo->Reset(BlockIo, FALSE);
            status = BlockIo->WriteBlocks(BlockIo, BlockIo->Media->MediaId, lba, size, buffer);
        }
        return (status == EFI_SUCCESS);
    }

    static bool zero_lba_range(EFI_BLOCK_IO_PROTOCOL* BlockIo, uint64_t start_lba, uint32_t sector_count, uint8_t* zero_buf, uint32_t zero_buf_sectors) {
        uint32_t rem = sector_count;
        uint64_t cur = start_lba;
        while (rem > 0) {
            uint32_t chunk = (rem > zero_buf_sectors) ? zero_buf_sectors : rem;
            if (!write_blocks(BlockIo, cur, chunk * 512U, zero_buf)) return false;
            cur += chunk;
            rem -= chunk;
        }
        return true;
    }

    bool format_disk_gpt_fat32(EFI_SYSTEM_TABLE *SystemTable, EFI_BLOCK_IO_PROTOCOL* BlockIo) {
        if (!SystemTable || !BlockIo || !BlockIo->Media || BlockIo->Media->ReadOnly) return false;

        uint32_t block_size = BlockIo->Media->BlockSize;
        if (block_size != 512) return false;

        uint64_t last_lba = BlockIo->Media->LastBlock;
        if (last_lba < 80000) return false; // Needs >= ~40 MB so FAT32 (>= 65525 clusters) is spec-compliant

        // Allocate 16 pages (64KB) aligned scratch memory
        EFI_PHYSICAL_ADDRESS scratch_phys = 0;
        EFI_STATUS status = SystemTable->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, 16, &scratch_phys);
        if (EFI_ERROR(status) || scratch_phys == 0) return false;

        uint8_t* sector_buf = (uint8_t*)scratch_phys;              // 512 bytes (padded to 4KB)
        uint8_t* part_array = (uint8_t*)(scratch_phys + 4096);      // 16384 bytes (32 sectors)
        uint8_t* zeros      = (uint8_t*)(scratch_phys + 20480);     // 32768 bytes = 64 sectors zero buffer
        const uint32_t zero_sectors = 64;
        memset8(zeros, 0, (int)(zero_sectors * 512U));

        // ----------------------------------------------------
        // 1. Protective MBR (LBA 0)
        // ----------------------------------------------------
        memset8(sector_buf, 0, 512);
        MasterBootRecord* mbr = (MasterBootRecord*)sector_buf;
        mbr->unique_mbr_signature = 0x4B4E4C42U;
        mbr->partitions[0].boot_indicator = 0;
        mbr->partitions[0].start_head = 0;
        mbr->partitions[0].start_sector = 2;
        mbr->partitions[0].start_track = 0;
        mbr->partitions[0].os_type = 0xEE; // GPT Protective
        mbr->partitions[0].end_head = 0xFF;
        mbr->partitions[0].end_sector = 0xFF;
        mbr->partitions[0].end_track = 0xFF;
        mbr->partitions[0].starting_lba = 1;
        mbr->partitions[0].size_in_lba = (last_lba > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)last_lba;
        mbr->signature = 0xAA55;
        if (!write_blocks(BlockIo, 0, 512, sector_buf)) {
            SystemTable->BootServices->FreePages(scratch_phys, 16);
            return false;
        }

        // ----------------------------------------------------
        // 2. GPT Partition Entries (LBA 2..33 & Backup at last_lba - 32)
        // ----------------------------------------------------
        uint64_t first_usable_lba = 34;
        uint64_t last_usable_lba = last_lba - 33;
        uint64_t partition_start = 2048; // 1MB aligned
        if (partition_start < first_usable_lba) partition_start = first_usable_lba;
        uint64_t partition_end = last_usable_lba;
        // Cap single FAT32 partition size to < 2TB (0xFFFFFFFF sectors)
        if (partition_end - partition_start + 1ULL > 0xFFFFFFFFULL) {
            partition_end = partition_start + 0xFFFFFFFEULL;
        }
        uint64_t partition_size_lba = partition_end - partition_start + 1ULL;

        EFI_GUID esp_guid = {0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};
        uint32_t seed = (uint32_t)(last_lba ^ (uintptr_t)BlockIo);
        EFI_GUID unique_guid = {0x514B0000U ^ seed, 0xABCD, 0x4E01, {0x93, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, (uint8_t)(seed & 0xFFU)}};

        memset8(part_array, 0, 16384);
        GptPartitionEntry* entry = (GptPartitionEntry*)part_array;
        entry->partition_type_guid = esp_guid;
        entry->unique_partition_guid = unique_guid;
        entry->starting_lba = partition_start;
        entry->ending_lba = partition_end;
        entry->attributes = 0;

        const CHAR16* name = (const CHAR16*)L"BlankOS EFI";
        for (int i = 0; name[i]; i++) entry->partition_name[i] = name[i];

        uint32_t part_array_crc = calculate_crc32(part_array, 16384);

        if (!write_blocks(BlockIo, 2, 16384, part_array)) {
            SystemTable->BootServices->FreePages(scratch_phys, 16);
            return false;
        }
        write_blocks(BlockIo, last_lba - 32, 16384, part_array);

        // ----------------------------------------------------
        // 3. Primary & Backup GPT Headers
        // ----------------------------------------------------
        memset8(sector_buf, 0, 512);
        GptHeader* gpt = (GptHeader*)sector_buf;
        gpt->signature = 0x5452415020494645ULL; // "EFI PART"
        gpt->revision = 0x00010000U;
        gpt->header_size = 92;
        gpt->my_lba = 1;
        gpt->alternate_lba = last_lba;
        gpt->first_usable_lba = first_usable_lba;
        gpt->last_usable_lba = last_usable_lba;

        EFI_GUID disk_guid = {0x87654321U ^ seed, 0x4321, 0x4321, {0x81, 0x43, 0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB}};
        gpt->disk_guid = disk_guid;
        gpt->partition_entry_lba = 2;
        gpt->num_partition_entries = 128;
        gpt->size_of_partition_entry = 128;
        gpt->partition_entry_array_crc32 = part_array_crc;
        gpt->header_crc32 = 0;
        gpt->header_crc32 = calculate_crc32((uint8_t*)gpt, 92);

        if (!write_blocks(BlockIo, 1, 512, sector_buf)) {
            SystemTable->BootServices->FreePages(scratch_phys, 16);
            return false;
        }

        gpt->my_lba = last_lba;
        gpt->alternate_lba = 1;
        gpt->partition_entry_lba = last_lba - 32;
        gpt->header_crc32 = 0;
        gpt->header_crc32 = calculate_crc32((uint8_t*)gpt, 92);
        write_blocks(BlockIo, last_lba, 512, sector_buf);

        // ----------------------------------------------------
        // 4. Adaptive FAT32 Sizing (guarantees >= 65525 clusters per FAT32 spec)
        // ----------------------------------------------------
        uint16_t reserved = 32;
        uint8_t spc = 8; // 4KB default
        if (partition_size_lba < 560000ULL) {
            spc = 1; // < 273 MB -> 512B clusters so clusters >= 65525
        } else if (partition_size_lba < 16777216ULL) {
            spc = 8; // < 8 GB -> 4KB clusters
        } else if (partition_size_lba < 33554432ULL) {
            spc = 16; // < 16 GB -> 8KB clusters
        } else {
            spc = 32;
        }

        uint64_t tmp1 = partition_size_lba - reserved;
        uint64_t tmp2 = (256ULL * (uint64_t)spc) + 2ULL;
        uint32_t fat_size = (uint32_t)((tmp1 + (tmp2 - 1ULL)) / tmp2);
        uint64_t data_sectors = partition_size_lba - reserved - 2ULL * (uint64_t)fat_size;
        uint64_t total_clusters = data_sectors / (uint64_t)spc;
        if (total_clusters < 65525ULL && spc > 1) {
            spc = 1;
            tmp2 = (256ULL * (uint64_t)spc) + 2ULL;
            fat_size = (uint32_t)((tmp1 + (tmp2 - 1ULL)) / tmp2);
            data_sectors = partition_size_lba - reserved - 2ULL * (uint64_t)fat_size;
            total_clusters = data_sectors / (uint64_t)spc;
        }
        if (total_clusters < 65525ULL) {
            SystemTable->BootServices->FreePages(scratch_phys, 16);
            return false;
        }

        // Zero entire reserved region first (clears backup sectors cleanly)
        if (!zero_lba_range(BlockIo, partition_start, reserved, zeros, zero_sectors)) {
            SystemTable->BootServices->FreePages(scratch_phys, 16);
            return false;
        }

        // ----------------------------------------------------
        // 5. FAT32 Volume Boot Record (VBR + Backup VBR at +6)
        // ----------------------------------------------------
        memset8(sector_buf, 0, 512);
        Fat32BootSector* vbr = (Fat32BootSector*)sector_buf;
        vbr->jmp_boot[0] = 0xEB; vbr->jmp_boot[1] = 0x58; vbr->jmp_boot[2] = 0x90;

        const char* oem = "MSWIN4.1";
        for (int i = 0; i < 8; i++) vbr->oem_name[i] = oem[i];

        vbr->bytes_per_sector = 512;
        vbr->sectors_per_cluster = spc;
        vbr->reserved_sector_count = reserved;
        vbr->num_fats = 2;
        vbr->media = 0xF8;
        vbr->sectors_per_track = 63;
        vbr->number_of_heads = 255;
        vbr->hidden_sectors = (uint32_t)partition_start;
        vbr->total_sectors_32 = (uint32_t)partition_size_lba;
        vbr->fat_size_32 = fat_size;
        vbr->root_cluster = 2;
        vbr->fs_info = 1;
        vbr->backup_boot_sector = 6;
        vbr->drive_number = 0x80;
        vbr->boot_signature = 0x29;
        vbr->volume_id = 0x424C4E4BU; // "BLNK"

        const char* vol_label = "BLANKOS    ";
        for (int i = 0; i < 11; i++) vbr->volume_label[i] = vol_label[i];
        const char* fs_type = "FAT32   ";
        for (int i = 0; i < 8; i++) vbr->fs_type[i] = fs_type[i];
        vbr->boot_sector_signature = 0xAA55;

        if (!write_blocks(BlockIo, partition_start + 0, 512, sector_buf)) {
            SystemTable->BootServices->FreePages(scratch_phys, 16);
            return false;
        }
        write_blocks(BlockIo, partition_start + 6, 512, sector_buf);

        // ----------------------------------------------------
        // 6. FAT32 FSInfo Sector (+1 and Backup +7)
        // ----------------------------------------------------
        memset8(sector_buf, 0, 512);
        Fat32FsInfo* fsinfo = (Fat32FsInfo*)sector_buf;
        fsinfo->lead_sig = 0x41615252U;
        fsinfo->struc_sig = 0x61417272U;
        fsinfo->free_count = (total_clusters > 1) ? (uint32_t)(total_clusters - 1ULL) : 0xFFFFFFFFU;
        fsinfo->nxt_free = 3;
        fsinfo->trail_sig = 0xAA550000U;

        write_blocks(BlockIo, partition_start + 1, 512, sector_buf);
        write_blocks(BlockIo, partition_start + 7, 512, sector_buf);

        // ----------------------------------------------------
        // 7. Zero BOTH complete FAT Tables + Write Cluster 0..2 Entries
        // ----------------------------------------------------
        uint64_t fat1_start = partition_start + reserved;
        uint64_t fat2_start = fat1_start + (uint64_t)fat_size;

        if (!zero_lba_range(BlockIo, fat1_start, fat_size, zeros, zero_sectors) ||
            !zero_lba_range(BlockIo, fat2_start, fat_size, zeros, zero_sectors)) {
            SystemTable->BootServices->FreePages(scratch_phys, 16);
            return false;
        }

        memset8(sector_buf, 0, 512);
        uint32_t* fat_entries = (uint32_t*)sector_buf;
        fat_entries[0] = 0x0FFFFFF8U;
        fat_entries[1] = 0x0FFFFFFFU;
        fat_entries[2] = 0x0FFFFFFFU; // Root Directory EOC

        if (!write_blocks(BlockIo, fat1_start, 512, sector_buf) ||
            !write_blocks(BlockIo, fat2_start, 512, sector_buf)) {
            SystemTable->BootServices->FreePages(scratch_phys, 16);
            return false;
        }

        // ----------------------------------------------------
        // 8. Zero Root Directory Cluster (Cluster 2)
        // ----------------------------------------------------
        uint64_t data_start = fat2_start + (uint64_t)fat_size;
        zero_lba_range(BlockIo, data_start, (uint32_t)spc, zeros, zero_sectors);

        if (BlockIo->FlushBlocks) {
            BlockIo->FlushBlocks(BlockIo);
        }

        SystemTable->BootServices->FreePages(scratch_phys, 16);
        return true;
    }
}
