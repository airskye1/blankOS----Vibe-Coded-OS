#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <efi.h>
#include "../sdk/blankos.h"

extern "C" {
    extern void dui_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha);
    extern void dui_text(int x, int y, const char* text, uint32_t color, int scale);
    extern int dui_text_width(const char* text, int scale);
    extern void swap_buffers();
    extern void play_system_sound(char* sound_name);
    extern int screen_width;
    extern int screen_height;
    
    // Exception frame structure passed from assembly
    struct CpuState {
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
        uint64_t exception_num, error_code;
        uint64_t rip, cs, rflags, rsp, ss;
    };
    
    // IDT gate descriptor structure for x86_64
    struct IDTEntry {
        uint16_t offset_low;
        uint16_t selector;
        uint8_t  ist;
        uint8_t  type_attributes;
        uint16_t offset_mid;
        uint32_t offset_high;
        uint32_t zero;
    } __attribute__((packed));
    
    struct IDTR {
        uint16_t limit;
        uint64_t base;
    } __attribute__packed__;
    
    static IDTEntry idt[256];
    static IDTR idtr;
    
    // Diagnostic messages for exceptions
    static const char* exception_names[] = {
        "Divide-by-Zero Exception (#DE)",
        "Debug Exception (#DB)",
        "Non-Maskable Interrupt (#NMI)",
        "Breakpoint Exception (#BP)",
        "Overflow Exception (#OF)",
        "Bound Range Exceeded (#BR)",
        "Invalid Opcode Exception (#UD)",
        "Device Not Available (#NM)",
        "Double Fault Exception (#DF)",
        "Coprocessor Segment Overrun",
        "Invalid TSS Exception (#TS)",
        "Segment Not Present (#NP)",
        "Stack-Segment Fault (#SS)",
        "General Protection Fault (#GP)",
        "Page Fault Exception (#PF)",
        "Reserved Exception",
        "x87 Floating-Point Exception (#MF)",
        "Alignment Check Exception (#AC)",
        "Machine Check Exception (#MC)",
        "SIMD Floating-Point Exception (#XM)",
        "Virtualization Exception (#VE)",
        "Control Protection Exception (#CP)"
    };
    
    // Hexadecimal string helper
    static void uint64_to_hex(uint64_t val, char* buf) {
        const char* hex_chars = "0123456789ABCDEF";
        buf[0] = '0';
        buf[1] = 'x';
        for (int i = 0; i < 16; i++) {
            buf[2 + (15 - i)] = hex_chars[(val >> (i * 4)) & 0x0F];
        }
        buf[18] = '\0';
    }

    static void uint64_to_dec(uint64_t val, char* buf) {
        if (val == 0) {
            buf[0] = '0';
            buf[1] = '\0';
            return;
        }
        char temp[24];
        int i = 0;
        while (val > 0) {
            temp[i++] = '0' + (val % 10);
            val /= 10;
        }
        int j = 0;
        while (i > 0) {
            buf[j++] = temp[--i];
        }
        buf[j] = '\0';
    }
    
    // The actual hardware exception panic display
    void blankOS_panic_handler(CpuState* state) {
        // Stop sound
        play_system_sound((char*)"error");
        
        // Render styled macOS Black Screen of Death panic panel
        dui_rect(0, 0, screen_width, screen_height, 0x0A0A0A, 255); // Jet black
        
        int box_w = 780;
        int box_h = 560;
        int box_x = (screen_width - box_w) / 2;
        int box_y = (screen_height - box_h) / 2;
        
        // Inner translucent box
        dui_rect(box_x, box_y, box_w, box_h, 0x1A1A1A, 255);
        
        // Diagnostic Header
        dui_text(box_x + 40, box_y + 40, "Your system encountered a fatal exception and had to halt.", 0xFF3B30, 2); // Bright red
        dui_text(box_x + 40, box_y + 80, "Kernel Panic Diagnostic Information:", 0xFFFFFF, 1);
        
        // Print Exception type
        char temp_buf[64];
        const char* exc_name = "Unknown Exception";
        if (state->exception_num < 22) {
            exc_name = exception_names[state->exception_num];
        }
        dui_text(box_x + 40, box_y + 110, "Exception type: ", 0x999999, 1);
        dui_text(box_x + 180, box_y + 110, exc_name, 0xFFCC00, 1);
        
        // Dump instruction pointer and stack pointer
        char rip_str[32], rsp_str[32], cr2_str[32];
        uint64_to_hex(state->rip, rip_str);
        uint64_to_hex(state->rsp, rsp_str);
        
        dui_text(box_x + 40, box_y + 140, "Faulting Instruction (RIP):", 0x999999, 1);
        dui_text(box_x + 300, box_y + 140, rip_str, 0xFFFFFF, 1);
        
        dui_text(box_x + 40, box_y + 160, "Stack Pointer (RSP):", 0x999999, 1);
        dui_text(box_x + 300, box_y + 160, rsp_str, 0xFFFFFF, 1);
        
        // For Page Fault, display CR2 address
        if (state->exception_num == 14) {
            uint64_t cr2_val;
            asm volatile("mov %%cr2, %0" : "=r"(cr2_val));
            uint64_to_hex(cr2_val, cr2_str);
            dui_text(box_x + 40, box_y + 180, "Faulting Linear Address (CR2):", 0xFF3B30, 1);
            dui_text(box_x + 300, box_y + 180, cr2_str, 0xFFFFFF, 1);
        }
        
        // CPU Registers Dump
        dui_text(box_x + 40, box_y + 220, "--- CPU General Purpose Registers ---", 0x999999, 1);
        
        char reg_str[32];
        struct RegInfo { const char* name; uint64_t val; };
        RegInfo regs[] = {
            {"RAX", state->rax}, {"RBX", state->rbx}, {"RCX", state->rcx}, {"RDX", state->rdx},
            {"RSI", state->rsi}, {"RDI", state->rdi}, {"RBP", state->rbp}, {"R8 ", state->r8},
            {"R9 ", state->r9},  {"R10", state->r10}, {"R11", state->r11}, {"R12", state->r12},
            {"R13", state->r13}, {"R14", state->r14}, {"R15", state->r15}, {"ERR", state->error_code}
        };
        
        for (int i = 0; i < 16; i++) {
            int rx = box_x + 40 + (i % 2) * 360;
            int ry = box_y + 250 + (i / 2) * 24;
            uint64_to_hex(regs[i].val, reg_str);
            dui_text(rx, ry, regs[i].name, 0x8A8A8F, 1);
            dui_text(rx + 50, ry, reg_str, 0x34C759, 1); // green values
        }
        
        dui_text(box_x + 40, box_y + 460, "The CPU has been safely halted to protect filesystem integrity.", 0x999999, 1);
        dui_text(box_x + 40, box_y + 490, "Please restart or reset the virtual machine to reload BlankOS.", 0xFFCC00, 1);
        
        swap_buffers();
        
        // Loop forever and halt
        while(1) {
            asm volatile("cli; hlt");
        }
    }
    
    // Panic system call (invoked by apps or shell)
    void blankOS_panic(const char* error_code, const char* details) {
        CpuState state = {0};
        state.exception_num = 15; // Reserved/Custom Software Panic
        state.rip = (uintptr_t)__builtin_return_address(0);
        
        // Trigger the handler directly
        blankOS_panic_handler(&state);
    }
    
    // Exception Interrupt Entry Handlers definitions
    extern void idt_div_by_zero();
    extern void idt_invalid_opcode();
    extern void idt_gpf();
    extern void idt_page_fault();
    
    static void set_idt_gate(int num, uint64_t handler_addr) {
        idt[num].offset_low = (uint16_t)(handler_addr & 0xFFFF);
        idt[num].selector = 0x08; // Kernel code segment
        idt[num].ist = 0;
        idt[num].type_attributes = 0x8E; // 32-bit Interrupt Gate, Ring 0 present
        idt[num].offset_mid = (uint16_t)((handler_addr >> 16) & 0xFFFF);
        idt[num].offset_high = (uint32_t)((handler_addr >> 32) & 0xFFFFFFFF);
        idt[num].zero = 0;
    }
    
    void init_idt() {
        // Initialise all entries to 0/empty
        for (int i = 0; i < 256; i++) {
            idt[i].offset_low = 0;
            idt[i].selector = 0;
            idt[i].ist = 0;
            idt[i].type_attributes = 0;
            idt[i].offset_mid = 0;
            idt[i].offset_high = 0;
            idt[i].zero = 0;
        }
        
        // Map critical hardware exceptions
        set_idt_gate(0, (uint64_t)idt_div_by_zero);
        set_idt_gate(6, (uint64_t)idt_invalid_opcode);
        set_idt_gate(13, (uint64_t)idt_gpf);
        set_idt_gate(14, (uint64_t)idt_page_fault);
        
        idtr.limit = (uint16_t)(sizeof(IDTEntry) * 256 - 1);
        idtr.base = (uint64_t)&idt;
        
        // Load IDT register
        asm volatile("lidt %0" : : "m"(idtr));
    }
}

// ---------------------------------------------------------
// Assembly Exception Handler Stubs using GCC assembly block
// ---------------------------------------------------------
__asm__(
    ".global idt_div_by_zero\n"
    "idt_div_by_zero:\n"
    "pushq $0\n" // Dummy error code
    "pushq $0\n" // Exception 0
    "jmp common_exception_handler\n"
    
    ".global idt_invalid_opcode\n"
    "idt_invalid_opcode:\n"
    "pushq $0\n" // Dummy error code
    "pushq $6\n" // Exception 6
    "jmp common_exception_handler\n"
    
    ".global idt_gpf\n"
    "idt_gpf:\n" // GPF has its own error code pushed by CPU
    "pushq $13\n" // Exception 13
    "jmp common_exception_handler\n"
    
    ".global idt_page_fault\n"
    "idt_page_fault:\n" // Page fault has its own error code pushed by CPU
    "pushq $14\n" // Exception 14
    "jmp common_exception_handler\n"
    
    "common_exception_handler:\n"
    // Push general purpose registers to match CpuState struct layout
    "pushq %rax\n"
    "pushq %rbx\n"
    "pushq %rcx\n"
    "pushq %rdx\n"
    "pushq %rsi\n"
    "pushq %rdi\n"
    "pushq %rbp\n"
    "pushq %r8\n"
    "pushq %r9\n"
    "pushq %r10\n"
    "pushq %r11\n"
    "pushq %r12\n"
    "pushq %r13\n"
    "pushq %r14\n"
    "pushq %r15\n"
    
    // Pass pointer to registers (current stack pointer RSP) as 1st argument in SysV call (RDI)
    "movq %rsp, %rdi\n"
    
    // Align stack to 16 bytes before calling C code (firmware safety)
    "movq %rsp, %rax\n"
    "andq $-16, %rsp\n"
    "pushq %rax\n" // Save original stack pointer
    
    "call blankOS_panic_handler\n"
);
