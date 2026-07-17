#include "i_system.h"
#include "d_event.h"
#include "d_net.h"
#include "m_argv.h"
#include "doomstat.h"
#include "i_net.h"

extern "C" {
    void I_InitNetwork(void) {
        doomcom = (doomcom_t*)malloc(sizeof(doomcom_t));
        memset(doomcom, 0, sizeof(doomcom_t));
        
        // Single player mode configurations
        doomcom->ticdup = 1;
        doomcom->extratics = 0;
        doomcom->numnodes = 1;
        doomcom->numplayers = 1;
        doomcom->consoleplayer = 0;
        doomcom->id = DOOMCOM_ID;
        
        // Disable network packets hooks
        netgame = false;
        multiplayer = false;
    }

    void I_NetCmd(void) {
        if (doomcom->command == CMD_SEND) {
            // Loopback packets immediately in single player mode
            doomcom->remotetic = doomcom->gametic;
        } else if (doomcom->command == CMD_GET) {
            // No incoming net packets
            doomcom->remotetic = -1;
        }
    }
}
