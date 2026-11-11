#include <stdint.h>

#define BLOE_MAGIC 0x424C4F45 // 'BLOE'

typedef struct {
    uint32_t magic;
    uint32_t entry_point;
    uint32_t text_segment_offset;
    uint32_t data_segment_offset;
    uint32_t bss_size;
} bloe_header;

extern "C" {
extern void* vm_allocate(uint32_t size);

void load_bloe_executable(uint8_t* file_buffer) {
    bloe_header* header = (bloe_header*)file_buffer;
    
    if (header->magic != BLOE_MAGIC) {
        return;
    }
    
    void* text_seg = vm_allocate(header->data_segment_offset - header->text_segment_offset);
    (void)text_seg;
}
}
