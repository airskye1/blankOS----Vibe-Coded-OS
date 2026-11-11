#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include <efilib.h>

#define HEAP_POOL_SIZE (4ULL * 1024ULL * 1024ULL)            // 4 MB UEFI Heap Pool
#define STATIC_FALLBACK_HEAP_SIZE (512ULL * 1024ULL)         // 512 KB Static Pool before EFI init
#define HEAP_BLOCK_MAGIC 0xB1A0C0DE

// Free-list block header for genuine dynamic memory allocation, splitting, and coalescing
typedef struct HeapBlock {
    uint64_t size;
    uint32_t magic;
    uint32_t is_free;
    struct HeapBlock* next;
    struct HeapBlock* prev;
} HeapBlock;

static uint8_t static_fallback_heap[STATIC_FALLBACK_HEAP_SIZE] __attribute__((aligned(16)));
static uint8_t mmap_scratch_buf[32768] __attribute__((aligned(16)));

static uint8_t* heap_base = NULL;
static uint64_t heap_capacity = 0;
static HeapBlock* free_list_head = NULL;

static uint64_t heap_used_bytes = 0;
static uint32_t alloc_count = 0;

static uint64_t total_physical_ram_kb = 0;
static uint64_t usable_conventional_ram_kb = 0;
static uint64_t uefi_allocated_ram_kb = 0;
static bool heap_initialized = false;

static void setup_heap_region(uint8_t* base, uint64_t capacity) {
    heap_base = base;
    heap_capacity = capacity;
    heap_used_bytes = 0;

    free_list_head = (HeapBlock*)heap_base;
    free_list_head->size = capacity - sizeof(HeapBlock);
    free_list_head->magic = HEAP_BLOCK_MAGIC;
    free_list_head->is_free = 1;
    free_list_head->next = NULL;
    free_list_head->prev = NULL;
    heap_initialized = true;
}

void init_memory(void) {
    if (!heap_initialized) {
        setup_heap_region(static_fallback_heap, STATIC_FALLBACK_HEAP_SIZE);
    }
}

void* kmalloc(uint64_t size) {
    if (!heap_initialized) {
        init_memory();
    }
    if (size == 0) size = 16;
    uint64_t aligned_size = (size + 15ULL) & ~15ULL;

    HeapBlock* curr = free_list_head;
    while (curr != NULL) {
        if (curr->magic == HEAP_BLOCK_MAGIC && curr->is_free && curr->size >= aligned_size) {
            // Split block if enough space remains for another HeapBlock header + 32 bytes payload
            if (curr->size >= aligned_size + sizeof(HeapBlock) + 32ULL) {
                HeapBlock* next_block = (HeapBlock*)((uint8_t*)curr + sizeof(HeapBlock) + aligned_size);
                next_block->size = curr->size - aligned_size - sizeof(HeapBlock);
                next_block->magic = HEAP_BLOCK_MAGIC;
                next_block->is_free = 1;
                next_block->next = curr->next;
                next_block->prev = curr;
                if (curr->next) {
                    curr->next->prev = next_block;
                }
                curr->next = next_block;
                curr->size = aligned_size;
            }

            curr->is_free = 0;
            heap_used_bytes += curr->size;
            alloc_count++;

            uint8_t* payload = (uint8_t*)curr + sizeof(HeapBlock);
            for (uint64_t i = 0; i < aligned_size; i++) {
                payload[i] = 0;
            }
            return (void*)payload;
        }
        curr = curr->next;
    }
    return NULL;
}

void kfree(void* ptr) {
    if (!ptr || !heap_initialized) return;
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)heap_base;
    if (addr < base + sizeof(HeapBlock) || addr >= base + heap_capacity) {
        return;
    }

    HeapBlock* block = (HeapBlock*)(addr - sizeof(HeapBlock));
    if (block->magic != HEAP_BLOCK_MAGIC || block->is_free) {
        return;
    }

    block->is_free = 1;
    if (heap_used_bytes >= block->size) {
        heap_used_bytes -= block->size;
    } else {
        heap_used_bytes = 0;
    }

    // Coalesce with next block if adjacent and free
    if (block->next && block->next->magic == HEAP_BLOCK_MAGIC && block->next->is_free) {
        HeapBlock* next_b = block->next;
        block->size += sizeof(HeapBlock) + next_b->size;
        block->next = next_b->next;
        if (next_b->next) {
            next_b->next->prev = block;
        }
    }

    // Coalesce with previous block if adjacent and free
    if (block->prev && block->prev->magic == HEAP_BLOCK_MAGIC && block->prev->is_free) {
        HeapBlock* prev_b = block->prev;
        prev_b->size += sizeof(HeapBlock) + block->size;
        prev_b->next = block->next;
        if (block->next) {
            block->next->prev = prev_b;
        }
    }
}

extern "C" {
    void init_memory_c(void) __asm__("init_memory");
    void init_memory_c(void) {
        init_memory();
    }

    void* kmalloc_c(uint64_t size) __asm__("kmalloc");
    void* kmalloc_c(uint64_t size) {
        return kmalloc(size);
    }

    void kfree_c(void* ptr) __asm__("kfree");
    void kfree_c(void* ptr) {
        kfree(ptr);
    }

    void memory_init_efi(EFI_SYSTEM_TABLE *SystemTable) {
        if (!SystemTable || !SystemTable->BootServices) {
            init_memory();
            return;
        }

        // 1. Allocate a 4MB kernel heap pool via UEFI AllocatePool
        VOID* pool_ptr = NULL;
        EFI_STATUS status = SystemTable->BootServices->AllocatePool(
            EfiLoaderData,
            (UINTN)HEAP_POOL_SIZE,
            &pool_ptr
        );
        if (!EFI_ERROR(status) && pool_ptr != NULL) {
            setup_heap_region((uint8_t*)pool_ptr, HEAP_POOL_SIZE);
        } else {
            setup_heap_region(static_fallback_heap, STATIC_FALLBACK_HEAP_SIZE);
        }

        // 2. Query UEFI Memory Map for real physical RAM and conventional usable RAM
        UINTN map_size = sizeof(mmap_scratch_buf);
        EFI_MEMORY_DESCRIPTOR* mem_map = (EFI_MEMORY_DESCRIPTOR*)mmap_scratch_buf;
        UINTN map_key = 0;
        UINTN desc_size = 0;
        UINT32 desc_version = 0;
        bool allocated_map_buf = false;

        status = SystemTable->BootServices->GetMemoryMap(
            &map_size,
            mem_map,
            &map_key,
            &desc_size,
            &desc_version
        );

        if (EFI_ERROR(status) && map_size > 0) {
            map_size += 4096;
            VOID* dyn_map = NULL;
            if (!EFI_ERROR(SystemTable->BootServices->AllocatePool(EfiLoaderData, map_size, &dyn_map)) && dyn_map != NULL) {
                mem_map = (EFI_MEMORY_DESCRIPTOR*)dyn_map;
                allocated_map_buf = true;
                status = SystemTable->BootServices->GetMemoryMap(
                    &map_size,
                    mem_map,
                    &map_key,
                    &desc_size,
                    &desc_version
                );
            }
        }

        if (!EFI_ERROR(status) && desc_size >= sizeof(EFI_MEMORY_DESCRIPTOR)) {
            uint64_t total_kb = 0;
            uint64_t free_kb = 0;
            uint64_t active_kb = 0;
            UINTN num_entries = map_size / desc_size;
            uint8_t* cursor = (uint8_t*)mem_map;

            for (UINTN i = 0; i < num_entries; i++) {
                EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)(cursor + (i * desc_size));
                uint64_t region_kb = desc->NumberOfPages * 4ULL; // 4 KB per UEFI page

                switch (desc->Type) {
                    case EfiConventionalMemory:
                        total_kb += region_kb;
                        free_kb += region_kb;
                        break;
                    case EfiLoaderCode:
                    case EfiLoaderData:
                    case EfiBootServicesCode:
                    case EfiBootServicesData:
                    case EfiRuntimeServicesCode:
                    case EfiRuntimeServicesData:
                    case EfiACPIReclaimMemory:
                    case EfiACPIMemoryNVS:
                    case EfiPersistentMemory:
                        total_kb += region_kb;
                        active_kb += region_kb;
                        break;
                    default:
                        break;
                }
            }

            if (total_kb > 0) {
                total_physical_ram_kb = total_kb;
                usable_conventional_ram_kb = free_kb;
                uefi_allocated_ram_kb = active_kb;
            }
        }

        if (allocated_map_buf && mem_map) {
            SystemTable->BootServices->FreePool((VOID*)mem_map);
        }
    }

    uint64_t memory_get_total_mb(void) {
        if (total_physical_ram_kb > 0) {
            uint64_t mb = (total_physical_ram_kb + 512ULL) / 1024ULL;
            return (mb > 0) ? mb : 1ULL;
        }
        return (heap_capacity + (1024ULL * 1024ULL - 1ULL)) / (1024ULL * 1024ULL);
    }

    uint64_t memory_get_used_mb(void) {
        uint64_t total_mb = memory_get_total_mb();
        uint64_t used_kb = uefi_allocated_ram_kb + (heap_used_bytes / 1024ULL);
        uint64_t used_mb = (used_kb + 512ULL) / 1024ULL;
        if (used_mb == 0) used_mb = 1ULL;
        if (used_mb > total_mb) used_mb = total_mb;
        return used_mb;
    }

    uint64_t memory_get_free_mb(void) {
        uint64_t total_mb = memory_get_total_mb();
        uint64_t used_mb = memory_get_used_mb();
        return (total_mb > used_mb) ? (total_mb - used_mb) : 0ULL;
    }

    uint32_t memory_get_alloc_count(void) {
        return alloc_count;
    }
}
