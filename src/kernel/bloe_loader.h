#ifndef BLOE_LOADER_H
#define BLOE_LOADER_H

#include <efi.h>
#include <stdbool.h>
#include <stdint.h>
#include "os_api.h"

extern "C" {
    void elf_init_os_api(EFI_SYSTEM_TABLE* SystemTable, OS_API* api);
    bool elf_run_from_memory(EFI_SYSTEM_TABLE* SystemTable, const uint8_t* file_data, uint64_t file_size);
    bool load_and_run_elf(EFI_SYSTEM_TABLE* SystemTable, CHAR16* filename);
    void load_bloe_executable(uint8_t* file_buffer);
    bool bloe_launch_file(EFI_SYSTEM_TABLE* SystemTable, const CHAR16* filename, char* out_status, int status_max);
}

#endif
