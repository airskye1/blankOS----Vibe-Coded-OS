#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>
#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"

extern "C" {
    int mb_used = 16; // allocate 16MB heap zone for Doom
    
    void I_Tactile(int on, int off, int total) {
        // No-op
    }

    ticcmd_t emptycmd;
    ticcmd_t* I_BaseTiccmd(void) {
        return &emptycmd;
    }

    int I_GetHeapSize(void) {
        return mb_used * 1024 * 1024;
    }

    byte* I_ZoneBase(int* size) {
        *size = mb_used * 1024 * 1024;
        byte* p = (byte*)malloc(*size);
        if (p) memset(p, 0, *size);
        return p;
    }

    int I_GetTime(void) {
        struct timeval tp;
        static int basetime = 0;
        gettimeofday(&tp, NULL);
        if (!basetime) basetime = tp.tv_sec;
        int newtics = (tp.tv_sec - basetime) * TICRATE + tp.tv_usec * TICRATE / 1000000;
        return newtics;
    }

    void I_Init(void) {
        I_InitSound();
    }

    void I_Quit(void) {
        D_QuitNetGame();
        I_ShutdownSound();
        I_ShutdownMusic();
        M_SaveDefaults();
        I_ShutdownGraphics();
        exit(0);
    }

    void I_WaitVBL(int count) {
        // Stall roughly: each VBL tic is 1/70th of a second
        // 1000000 / 70 = 14285 microseconds per tick
        // 14285 * 1000 = 14285000 nanoseconds / stall count
        // Stall count * 14 milliseconds
        // We'll stub or use clock timing loop
    }

    void I_BeginRead(void) {
        // No-op
    }

    void I_EndRead(void) {
        // No-op
    }

    byte* I_AllocLow(int length) {
        byte* mem = (byte*)malloc(length);
        if (mem) memset(mem, 0, length);
        return mem;
    }

    extern boolean demorecording;

    void I_Error(char *error, ...) {
        char buf[256];
        va_list argptr;
        va_start(argptr, error);
        vsnprintf(buf, sizeof(buf), error, argptr);
        va_end(argptr);
        
        printf("Doom System Error: %s\r\n", buf);
        
        if (demorecording) G_CheckDemoStatus();
        D_QuitNetGame();
        I_ShutdownGraphics();
        exit(-1);
    }
}
