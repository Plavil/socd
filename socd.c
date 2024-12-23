#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <termios.h>
#include <sys/epoll.h>
#include <time.h>

#define UP     0
#define LEFT   1
#define DOWN   2
#define RIGHT  3

#define result_msg(call, fmt, args...) \
    do { if ((call) < 0) { perror(fmt); exit(1); } } while (0)

#define result(call) result_msg(call, "call failed")

struct keystate { char pressed; int which; };

// Global context structure
static struct {
    char *wr_target, rd_target[275], running;
    int write_fd, read_fd, rl_keystates[4];
    struct keystate vr_keystates[4];
} context = {
    .running = 1,
    .wr_target = "/dev/uinput",
    .rd_target = { 0 },
    .rl_keystates = { 0 },
    .vr_keystates = {
        { 0, KEY_W },   // UP
        { 0, KEY_A },   // LEFT
        { 0, KEY_S },   // DOWN
        { 0, KEY_D },   // RIGHT
    }
};

const char *BY_ID = "/dev/input/by-id/";
const char *BY_PATH = "/dev/input/by-path/";

void sigint_handler(int sig);
void emit(int type, int code, int value);
void emit_all(void);
void setup_write(void);
void process_event(const struct input_event *ev);
int get_keyboard(const char *path);
int prompt_user(int max);

int main() {
    if (signal(SIGINT, sigint_handler) == SIG_ERR) {
        perror("Failed to set signal handler");
        exit(1);
    }

    if (geteuid() != 0) {
        fprintf(stderr, "This program requires sudo to access keyboard inputs\n");
        exit(1);
    }

    // Get keyboard device
    if (get_keyboard(BY_ID) && get_keyboard(BY_PATH)) {
        fprintf(stderr, "Failed to get keyboards\n");
        exit(1);   
    }

    setup_write();
    
    context.read_fd = open(context.rd_target, O_RDONLY | O_NONBLOCK);
    result(context.read_fd);

    struct termios t_attrs;
    tcgetattr(STDIN_FILENO, &t_attrs);
    t_attrs.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t_attrs);

    struct input_event ev[64];

    while (context.running) {
        usleep(1000); // Sleep for 1 ms to reduce CPU usage
       
        int bytes_read = read(context.read_fd, ev, sizeof(ev));
        if (bytes_read < 0) {
            if (errno == EAGAIN) {
                // No input available, continue
                continue;
            }
            perror("Failed to read input");
            continue;
        }

        // Process each event if bytes were read
        for (int i = 0; i < bytes_read / sizeof(struct input_event); ++i) {
            process_event(&ev[i]);
        }

        emit_all();  // Emit state changes after processing events
    }

    result(ioctl(context.write_fd, UI_DEV_DESTROY));
    close(context.write_fd);
    close(context.read_fd);

    // Restore terminal attributes
    t_attrs.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t_attrs);
}

void sigint_handler(int sig) {
    (void)sig; // Unused
    context.running = 0;
}

void setup_write() {
    context.write_fd = open(context.wr_target, O_WRONLY | O_NONBLOCK);
    result(context.write_fd);

    result(ioctl(context.write_fd, UI_SET_EVBIT, EV_KEY));
    for (int i = 0; i < 4; i++) {
        result(ioctl(context.write_fd, UI_SET_KEYBIT, context.vr_keystates[i].which));
    }

    struct uinput_setup setup = { .name = "socd_cleaner", .id = { .bustype = BUS_USB, .vendor = 0x1234, .product = 0x5678 } };
    result(ioctl(context.write_fd, UI_DEV_SETUP, &setup));
    result(ioctl(context.write_fd, UI_DEV_CREATE));
}

void process_event(const struct input_event *ev) {
    if (ev->value == 1) { // Key down
        switch (ev->code) {
        case KEY_W:
            if (context.rl_keystates[DOWN]) context.vr_keystates[DOWN].pressed = 0; // Prevent conflict
            context.rl_keystates[UP] = 1; context.vr_keystates[UP].pressed = 1; // Key pressed
            break;
        case KEY_A:
            if (context.rl_keystates[RIGHT]) context.vr_keystates[RIGHT].pressed = 0; // Prevent conflict
            context.rl_keystates[LEFT] = 1; context.vr_keystates[LEFT].pressed = 1; // Key pressed
            break;
        case KEY_S:
            if (context.rl_keystates[UP]) context.vr_keystates[UP].pressed = 0; // Prevent conflict
            context.rl_keystates[DOWN] = 1; context.vr_keystates[DOWN].pressed = 1; // Key pressed
            break;
        case KEY_D:
            if (context.rl_keystates[LEFT]) context.vr_keystates[LEFT].pressed = 0; // Prevent conflict
            context.rl_keystates[RIGHT] = 1; context.vr_keystates[RIGHT].pressed = 1; // Key pressed
            break;
        }
    } else if (ev->value == 0) { // Key up
        switch (ev->code) {
        case KEY_W:
            context.rl_keystates[UP] = 0; // Update the key state
            if (context.rl_keystates[DOWN]) context.vr_keystates[DOWN].pressed = 1; // Restore DOWN if still pressed
            context.vr_keystates[UP].pressed = 0; // Release state
            break;
        case KEY_A:
            context.rl_keystates[LEFT] = 0; // Update the key state
            if (context.rl_keystates[RIGHT]) context.vr_keystates[RIGHT].pressed = 1; // Restore RIGHT if still pressed
            context.vr_keystates[LEFT].pressed = 0; // Release state
            break;
        case KEY_S:
            context.rl_keystates[DOWN] = 0; // Update the key state
            if (context.rl_keystates[UP]) context.vr_keystates[UP].pressed = 1; // Restore UP if still pressed
            context.vr_keystates[DOWN].pressed = 0; // Release state
            break;
        case KEY_D:
            context.rl_keystates[RIGHT] = 0; // Update the key state
            if (context.rl_keystates[LEFT]) context.vr_keystates[LEFT].pressed = 1; // Restore LEFT if still pressed
            context.vr_keystates[RIGHT].pressed = 0; // Release state
            break;
        }
    }
}

void emit(int type, int code, int value) {
    struct input_event event = { .code = code, .type = type, .value = value, .time = {0, 0} };
    write(context.write_fd, &event, sizeof(event));
}

void emit_all() {
    for (int i = 0; i < 4; ++i) {
        emit(EV_KEY, context.vr_keystates[i].which, context.vr_keystates[i].pressed);
    }
    emit(EV_SYN, SYN_REPORT, 0); // Emit a SYN_REPORT to signal completion
}

int get_keyboard(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 1;

    strcpy(context.rd_target, path);
    char *possible_devices[8];
    int j = -1, selected = 0;
    struct dirent *dir;

    while ((dir = readdir(d)) != NULL && j < 7) {
        int len = strlen(dir->d_name);
        if (len >= 10 && strncmp(dir->d_name + len - 10, "-event-kbd", 10) == 0 && strncmp(dir->d_name + len - 15, "-if", 3) != 0) {
            possible_devices[++j] = dir->d_name;
        }
    }

    if (j == -1) return 1;

    if (j > 0) {
        selected = prompt_user(j);
    }

    strcat(context.rd_target, possible_devices[selected]);
    closedir(d);
    return 0;
}

int prompt_user(int max) {
    char c;
    while (1) {
        read(STDIN_FILENO, &c, 1);
        if (c < '1' || c > '0' + max) continue;
        break;
    }
    return c - '1';
}
