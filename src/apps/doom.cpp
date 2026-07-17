#include <efi.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern "C" {
    #include "doomdef.h"
    #include "d_main.h"
    
    // ---------------- Doom Source Amalgamation ----------------
    #include "doom_src/am_map.c"
    #include "doom_src/d_items.c"
    #include "doom_src/d_main.c"
    #include "doom_src/d_net.c"
    #include "doom_src/doomdef.c"
    #include "doom_src/doomstat.c"
    #include "doom_src/dstrings.c"
    #include "doom_src/f_finale.c"
    #include "doom_src/f_wipe.c"
    #include "doom_src/g_game.c"
    #include "doom_src/hu_lib.c"
    #include "doom_src/hu_stuff.c"
    #include "doom_src/info.c"
    #include "doom_src/m_argv.c"
    #include "doom_src/m_bbox.c"
    #include "doom_src/m_cheat.c"
    #include "doom_src/m_fixed.c"
    #include "doom_src/m_menu.c"
    #include "doom_src/m_misc.c"
    #include "doom_src/m_random.c"
    #include "doom_src/m_swap.c"
    
    #include "doom_src/p_ceilng.c"
    #include "doom_src/p_doors.c"
    #include "doom_src/p_enemy.c"
    #include "doom_src/p_floor.c"
    #include "doom_src/p_inter.c"
    #include "doom_src/p_lights.c"
    #include "doom_src/p_map.c"
    #include "doom_src/p_maputl.c"
    #include "doom_src/p_mobj.c"
    #include "doom_src/p_plats.c"
    #include "doom_src/p_pspr.c"
    #include "doom_src/p_saveg.c"
    #include "doom_src/p_setup.c"
    #include "doom_src/p_sight.c"
    #include "doom_src/p_spec.c"
    #include "doom_src/p_switch.c"
    #include "doom_src/p_telept.c"
    #include "doom_src/p_tick.c"
    #include "doom_src/p_user.c"
    
    #include "doom_src/r_bsp.c"
    #include "doom_src/r_data.c"
    #include "doom_src/r_draw.c"
    #include "doom_src/r_main.c"
    #include "doom_src/r_plane.c"
    #include "doom_src/r_segs.c"
    #include "doom_src/r_sky.c"
    #include "doom_src/r_things.c"
    
    #include "doom_src/sounds.c"
    #include "doom_src/st_lib.c"
    #include "doom_src/st_stuff.c"
    #include "doom_src/tables.c"
    #include "doom_src/v_video.c"
    #include "doom_src/w_wad.c"
    #include "doom_src/wi_stuff.c"
    #include "doom_src/z_zone.c"
    
    #include "doom_src/i_video.c"
    #include "doom_src/i_system.c"
    #include "doom_src/i_sound.c"
    #include "doom_src/i_net.c"

    // System table and game launcher
    extern EFI_SYSTEM_TABLE* global_ST;
    
    void launch_doom(EFI_SYSTEM_TABLE* SystemTable) {
        global_ST = SystemTable;
        
        static char* args[] = { (char*)"doom", (char*)"-nosound", (char*)"-nomusic", (char*)"-wad", (char*)"EFI\\APPS\\doom1.wad", NULL };
        myargc = 5;
        myargv = args;
        
        // Boot original Doom Main loop!
        D_DoomMain();
    }
}
