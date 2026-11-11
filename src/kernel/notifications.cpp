#include <stdint.h>
#include <stdbool.h>
#include "../ui/blankDUI.h"
#include "../ui/theme.h"

#define MAX_NOTIFICATIONS 8

typedef struct {
    char title[64];
    char message[128];
    int ticks_left; // ~60fps ticks
    float anim_slide;
    bool active;
} Notification;

static Notification notification_queue[MAX_NOTIFICATIONS];

static void copy_n(char* dst, const char* src, int max_len) {
    int i = 0;
    while (i < max_len - 1 && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

extern "C" {

void os_send_notification(const char* title, const char* message) {
    // Coalesce if a slot is free, otherwise evict oldest (index 0)
    int slot = -1;
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!notification_queue[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_NOTIFICATIONS - 1; i++) {
            notification_queue[i] = notification_queue[i + 1];
        }
        slot = MAX_NOTIFICATIONS - 1;
    }

    copy_n(notification_queue[slot].title, title ? title : "Horizon OS", 64);
    copy_n(notification_queue[slot].message, message ? message : "", 128);
    notification_queue[slot].ticks_left = 220; // ~3.6 seconds
    notification_queue[slot].anim_slide = 0.0f;
    notification_queue[slot].active = true;
}

void process_notifications(void) {
    int y_offset = (int)THEME_TOPBAR_H + 18;
    int shown = 0;
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!notification_queue[i].active) continue;

        Notification* n = &notification_queue[i];
        n->ticks_left--;
        if (n->ticks_left <= 0) {
            n->active = false;
            continue;
        }

        // Smooth slide-in / fade-out animation
        float target_slide = (n->ticks_left < 20) ? 0.0f : 1.0f;
        n->anim_slide = horizon_spring_step(n->anim_slide, target_slide, 0.22f);

        if (shown < 3) {
            int card_w = 328;
            int card_h = 66;
            int target_x = (int)screen_width - card_w - 18;
            int start_x = (int)screen_width + 20;
            int cur_x = start_x + (int)((target_x - start_x) * n->anim_slide);

            dui_shadow(cur_x, y_offset, card_w, card_h, THEME_RADIUS_LG, 8);
            dui_glass_card(cur_x, y_offset, card_w, card_h, THEME_RADIUS_LG, THEME_SURFACE_ELEVATED, 232, true);

            // Aurora indicator bar on left
            dui_rect_rounded(cur_x + 10, y_offset + 12, 4, card_h - 24, 2, THEME_ACCENT_CYAN, 240);

            dui_text_ellipsis(cur_x + 24, y_offset + 14, card_w - 40, n->title, THEME_TEXT_PRIMARY, 1);
            dui_text_ellipsis(cur_x + 24, y_offset + 38, card_w - 40, n->message, THEME_TEXT_SECONDARY, 1);

            y_offset += card_h + 10;
            shown++;
        }
    }
}

} // extern "C"
