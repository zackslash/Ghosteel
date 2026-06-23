/*
 * Sailfish OS Emulator Multi-Touch Drag Injector (Protocol-accurate)
 * Creates a virtual multitouch device and injects two-finger drag gestures
 *
 * Usage: mt_drag [start_x] [start_y] [end_x] [end_y] [steps] [delay_ms]
 * Default: drag from center (270,480) to bottom (270,900) over 60 steps
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/time.h>

#define SCREEN_W 540
#define SCREEN_H 960

static int uinput_fd = -1;

void emit(int fd, int type, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    gettimeofday(&ie.time, NULL);
    ie.type = type;
    ie.code = code;
    ie.value = val;
    write(fd, &ie, sizeof(ie));
}

void emit_syn(int fd) {
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

int setup_uinput() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/uinput");
        return -1;
    }

    // Enable event types
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    // Enable multitouch absolute axes
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MINOR);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOOL_TYPE);

    // Enable single-touch axes
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    // Enable key events
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_DOUBLETAP);

    // Use legacy uinput_user_dev struct
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Sailfish MultiTouch Emulator");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    // Set axis ranges
    uidev.absmin[ABS_MT_POSITION_X] = 0;
    uidev.absmax[ABS_MT_POSITION_X] = SCREEN_W;
    uidev.absmin[ABS_MT_POSITION_Y] = 0;
    uidev.absmax[ABS_MT_POSITION_Y] = SCREEN_H;

    uidev.absmin[ABS_MT_SLOT] = 0;
    uidev.absmax[ABS_MT_SLOT] = 9;

    uidev.absmin[ABS_MT_TRACKING_ID] = -1;
    uidev.absmax[ABS_MT_TRACKING_ID] = 65535;

    uidev.absmin[ABS_MT_TOUCH_MAJOR] = 0;
    uidev.absmax[ABS_MT_TOUCH_MAJOR] = 255;

    uidev.absmin[ABS_MT_TOUCH_MINOR] = 0;
    uidev.absmax[ABS_MT_TOUCH_MINOR] = 255;

    uidev.absmin[ABS_MT_TOOL_TYPE] = 0;
    uidev.absmax[ABS_MT_TOOL_TYPE] = 1;  // MT_TOOL_FINGER=0, MT_TOOL_PEN=1

    uidev.absmin[ABS_X] = 0;
    uidev.absmax[ABS_X] = SCREEN_W;
    uidev.absmin[ABS_Y] = 0;
    uidev.absmax[ABS_Y] = SCREEN_H;

    write(fd, &uidev, sizeof(uidev));

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    usleep(500000);
    return fd;
}

void send_two_finger_drag(int fd, int start_x, int start_y,
                           int end_x, int end_y, int steps, int delay_ms) {
    printf("Dragging two fingers: (%d,%d) -> (%d,%d) over %d steps (%dms delay)\n",
           start_x, start_y, end_x, end_y, steps, delay_ms);

    int finger_offset = 100;  // ~12mm apart, realistic finger spacing
    int touch_major = 40;     // ~4mm realistic contact size
    int inter_finger_delay = 25000;  // 25ms between finger 1 and finger 2 (realistic human)
    int baseline_frames = 3;  // Stationary frames after both fingers down

    // PHASE 1: Finger 0 down (staggered — realistic 10-50ms before finger 1)
    int baseline_x0 = start_x - finger_offset / 2;
    int baseline_x1 = start_x + finger_offset / 2;
    int baseline_y  = start_y;

    // Frame 1: Only finger 0 touches
    emit(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 0);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, baseline_x0);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, baseline_y);
    emit(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, touch_major);
    emit(fd, EV_ABS, ABS_MT_TOUCH_MINOR, touch_major);
    emit(fd, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
    emit(fd, EV_ABS, ABS_X, baseline_x0);
    emit(fd, EV_ABS, ABS_Y, baseline_y);
    emit(fd, EV_KEY, BTN_TOUCH, 1);
    emit(fd, EV_KEY, BTN_TOOL_FINGER, 1);  // Single finger initially
    emit_syn(fd);

    // Inter-finger delay (25ms - realistic human timing)
    usleep(inter_finger_delay);

    // PHASE 2: Finger 1 down — transition BTN_TOOL_FINGER → BTN_TOOL_DOUBLETAP
    // Frame 2: Finger 1 joins
    emit(fd, EV_ABS, ABS_MT_SLOT, 1);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, baseline_x1);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, baseline_y);
    emit(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, touch_major);
    emit(fd, EV_ABS, ABS_MT_TOUCH_MINOR, touch_major);
    emit(fd, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
    // Update slot 0 too (keep it current)
    emit(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, baseline_x0);
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, baseline_y);
    // Transition tool type
    emit(fd, EV_KEY, BTN_TOOL_FINGER, 0);     // No longer single-finger
    emit(fd, EV_KEY, BTN_TOOL_DOUBLETAP, 1);  // Now two-finger
    emit_syn(fd);

    // PHASE 3: Baseline frames (stationary, both fingers down)
    for (int b = 0; b < baseline_frames; b++) {
        emit(fd, EV_ABS, ABS_MT_SLOT, 0);
        emit(fd, EV_ABS, ABS_MT_POSITION_X, baseline_x0);
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, baseline_y);
        emit(fd, EV_ABS, ABS_MT_SLOT, 1);
        emit(fd, EV_ABS, ABS_MT_POSITION_X, baseline_x1);
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, baseline_y);
        emit_syn(fd);
        usleep(delay_ms > 0 ? delay_ms * 1000 : 16000);
    }

    // PHASE 4: Movement (both fingers same direction = scroll)
    for (int step = 1; step <= steps; step++) {
        float ratio = (float)step / steps;
        int current_x = start_x + (int)((end_x - start_x) * ratio);
        int current_y = start_y + (int)((end_y - start_y) * ratio);

        // Slot 0 (left finger)
        emit(fd, EV_ABS, ABS_MT_SLOT, 0);
        emit(fd, EV_ABS, ABS_MT_POSITION_X, current_x - finger_offset / 2);
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, current_y);

        // Slot 1 (right finger)
        emit(fd, EV_ABS, ABS_MT_SLOT, 1);
        emit(fd, EV_ABS, ABS_MT_POSITION_X, current_x + finger_offset / 2);
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, current_y);

        // Single-touch compat tracks slot 0
        emit(fd, EV_ABS, ABS_X, current_x - finger_offset / 2);
        emit(fd, EV_ABS, ABS_Y, current_y);

        emit_syn(fd);

        if (delay_ms > 0) {
            usleep(delay_ms * 1000);
        }
    }

    // PHASE 5: Finger release (staggered — finger 0 first, then finger 1)

    // Lift finger 0 (left) first
    emit(fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    // Transition: two-finger -> one-finger
    emit(fd, EV_KEY, BTN_TOOL_DOUBLETAP, 0);
    emit(fd, EV_KEY, BTN_TOOL_FINGER, 1);  // Still one finger down
    emit_syn(fd);
    usleep(30000);  // 30ms between finger lifts

    // Lift finger 1 (right) - now no fingers
    emit(fd, EV_ABS, ABS_MT_SLOT, 1);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(fd, EV_KEY, BTN_TOUCH, 0);
    emit(fd, EV_KEY, BTN_TOOL_FINGER, 0);
    emit_syn(fd);

    printf("Drag complete!\n");
}

void cleanup(int sig) {
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
    }
    exit(0);
}

int main(int argc, char *argv[]) {
    int start_x, start_y, end_x, end_y, steps, delay_ms;

    // Default: center to bottom
    start_x = SCREEN_W / 2;   // 270
    start_y = SCREEN_H / 2;   // 480
    end_x = SCREEN_W / 2;     // 270
    end_y = SCREEN_H - 60;    // 900
    steps = 60;
    delay_ms = 16;            // ~60fps

    if (argc >= 5) {
        start_x = atoi(argv[1]);
        start_y = atoi(argv[2]);
        end_x = atoi(argv[3]);
        end_y = atoi(argv[4]);
    }
    if (argc >= 6) steps = atoi(argv[5]);
    if (argc >= 7) delay_ms = atoi(argv[6]);

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    printf("Setting up multitouch device...\n");
    uinput_fd = setup_uinput();
    if (uinput_fd < 0) {
        fprintf(stderr, "Failed to create uinput device\n");
        return 1;
    }
    printf("Multitouch device created successfully!\n\n");

    send_two_finger_drag(uinput_fd, start_x, start_y, end_x, end_y, steps, delay_ms);

    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    printf("Device destroyed. Done.\n");

    return 0;
}
