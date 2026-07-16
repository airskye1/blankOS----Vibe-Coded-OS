#include <stdint.h>

#define BLOE_MAGIC 0x424C4F45 // 'BLOE'

typedef struct {
    uint32_t magic;
    uint32_t entry_point;
    uint32_t text_segment_offset;
    uint32_t data_segment_offset;
    uint32_t bss_size;
} bloe_header;

// Forward declaration for virtual memory mapper
extern void* vm_allocate(uint32_t size);

void load_bloe_executable(uint8_t* file_buffer) {
    bloe_header* header = (bloe_header*)file_buffer;
    
    if (header->magic != BLOE_MAGIC) {
        // Invalid executable
        return;
    }
    
    // 1. Allocate virtual memory for the process
    void* text_seg = vm_allocate(header->data_segment_offset - header->text_segment_offset);
    
    // 2. Copy the text segment (code) into memory
    // memcpy(text_seg, file_buffer + header->text_segment_offset, ...);
    
    // 3. Setup the process stack
    
    // 4. Context switch to ring 3 and jump to header->entry_point
}
