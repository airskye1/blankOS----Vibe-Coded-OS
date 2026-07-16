#include <stdint.h>
#include <stdbool.h>

#define MAX_NOTIFICATIONS 16

typedef struct {
    char title[64];
    char message[128];
    int duration_ms;
    bool active;
} Notification;

static Notification notification_queue[MAX_NOTIFICATIONS];
extern void blankUI_draw_toast(char* title, char* message);

// OS API for applications to trigger a notification
void os_send_notification(char* title, char* message) {
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!notification_queue[i].active) {
            // Copy strings securely here
            // strcpy(notification_queue[i].title, title);
            // strcpy(notification_queue[i].message, message);
            notification_queue[i].duration_ms = 3000;
            notification_queue[i].active = true;
            break;
        }
    }
}

// Called by the compositor loop every frame
void process_notifications(void) {
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (notification_queue[i].active) {
            // Render the toast via blankUI
            blankUI_draw_toast(notification_queue[i].title, notification_queue[i].message);
            
            // Decrease duration
            // notification_queue[i].duration_ms -= delta_time;
            // if (duration <= 0) notification_queue[i].active = false;
        }
    }
}
