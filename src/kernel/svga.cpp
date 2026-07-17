#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern "C" {
    extern uint32_t pci_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
    extern void pci_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t val);
    
    static inline void outl(uint16_t port, uint32_t val) {
        asm volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) );
    }

    static inline uint32_t inl(uint16_t port) {
        uint32_t ret;
        asm volatile ( "inl %1, %0" : "=a"(ret) : "Nd"(port) );
        return ret;
    }
}

// SVGA Registers
#define SVGA_REG_ID 0
#define SVGA_REG_ENABLE 1
#define SVGA_REG_WIDTH 2
#define SVGA_REG_HEIGHT 3
#define SVGA_REG_BITS_PER_PIXEL 7
#define SVGA_REG_CONFIG_DONE 20
#define SVGA_REG_SYNC 21

// FIFO Registers
#define SVGA_FIFO_MIN 0
#define SVGA_FIFO_MAX 1
#define SVGA_FIFO_NEXT_CMD 2
#define SVGA_FIFO_STOP 3

// SVGA Commands
#define SVGA_CMD_UPDATE 1
#define SVGA_CMD_RECT_FILL 2

#define SVGA_MAGIC 0x90000000
#define SVGA_MAKE_ID(ver) (SVGA_MAGIC << 8 | (ver))
#define SVGA_ID_2 SVGA_MAKE_ID(2)

extern "C" {
    bool svga_enabled = false;
    uint16_t svga_index_port = 0;
    uint16_t svga_value_port = 0;
    volatile uint32_t* svga_fifo = NULL;
    uint32_t* svga_fb = NULL;

    static void svga_write_reg(uint32_t index, uint32_t value) {
        outl(svga_index_port, index);
        outl(svga_value_port, value);
    }

    static uint32_t svga_read_reg(uint32_t index) {
        outl(svga_index_port, index);
        return inl(svga_value_port);
    }
    
    static void svga_fifo_sync() {
        if (!svga_enabled || !svga_fifo) return;
        svga_write_reg(SVGA_REG_SYNC, 1);
        while (svga_read_reg(SVGA_REG_SYNC) != 0) {
            // Wait for GPU to finish processing commands
        }
    }

    static void svga_fifo_write(uint32_t value) {
        if (!svga_enabled || !svga_fifo) return;
        uint32_t next_cmd = svga_fifo[SVGA_FIFO_NEXT_CMD];
        uint32_t max_cmd = svga_fifo[SVGA_FIFO_MAX];
        uint32_t min_cmd = svga_fifo[SVGA_FIFO_MIN];
        
        // Wait if full
        uint32_t next_next = next_cmd + 4;
        if (next_next == max_cmd) next_next = min_cmd;
        while (next_next == svga_fifo[SVGA_FIFO_STOP]) {
            svga_fifo_sync(); // sync to clear space
        }
        
        svga_fifo[next_cmd / 4] = value;
        svga_fifo[SVGA_FIFO_NEXT_CMD] = next_next;
    }

    bool svga_init(uint8_t bus, uint8_t device, uint8_t function) {
        uint32_t bar0 = pci_read_dword(bus, device, function, 0x10); // IO
        uint32_t bar1 = pci_read_dword(bus, device, function, 0x14); // FB
        uint32_t bar2 = pci_read_dword(bus, device, function, 0x18); // FIFO

        svga_index_port = (bar0 & 0xFFFC) + 0;
        svga_value_port = (bar0 & 0xFFFC) + 1;
        
        // Actually, VMware SVGA index is BAR0, value is BAR0+1 or BAR0+2 (typically IO ports)
        svga_index_port = (bar0 & 0xFFF0) + 0;
        svga_value_port = (bar0 & 0xFFF0) + 1;

        svga_write_reg(SVGA_REG_ID, SVGA_ID_2);
        if (svga_read_reg(SVGA_REG_ID) != SVGA_ID_2) {
            return false;
        }

        svga_fb = (uint32_t*)(uintptr_t)(bar1 & 0xFFFFFFF0);
        svga_fifo = (volatile uint32_t*)(uintptr_t)(bar2 & 0xFFFFFFF0);
        
        if (!svga_fifo) return false;

        svga_fifo[SVGA_FIFO_MIN] = 293 * 4; // Magic offset for SVGA FIFO start
        svga_fifo[SVGA_FIFO_MAX] = svga_read_reg(17); // SVGA_REG_MEM_SIZE
        svga_fifo[SVGA_FIFO_NEXT_CMD] = svga_fifo[SVGA_FIFO_MIN];
        svga_fifo[SVGA_FIFO_STOP] = svga_fifo[SVGA_FIFO_MIN];
        
        svga_write_reg(SVGA_REG_ENABLE, 1);
        svga_write_reg(SVGA_REG_CONFIG_DONE, 1);
        
        svga_enabled = true;
        return true;
    }

    void svga_update(int x, int y, int w, int h) {
        if (!svga_enabled) return;
        svga_fifo_write(SVGA_CMD_UPDATE);
        svga_fifo_write(x);
        svga_fifo_write(y);
        svga_fifo_write(w);
        svga_fifo_write(h);
        svga_fifo_sync();
    }

    void svga_fill_rect(int x, int y, int w, int h, uint32_t color) {
        if (!svga_enabled) return;
        svga_fifo_write(SVGA_CMD_RECT_FILL);
        svga_fifo_write(color);
        svga_fifo_write(x);
        svga_fifo_write(y);
        svga_fifo_write(w);
        svga_fifo_write(h);
        svga_fifo_sync();
    }
}
