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
#include <liburing.h>
#include <stdatomic.h>
#include <time.h>  // Include for usleep and nanosleep

#define UP     0
#define LEFT   1
#define DOWN   2
#define RIGHT  3
#define KEY_COUNT 4

#define result_msg(call, fmt, ...) \
    do { if ((call) < 0) { perror(fmt); exit(1); } } while (0)
#define result(call) result_msg(call, "call failed")

struct keystate { char pressed; int which; };

// Global context structure
static struct {
    char *wr_target, rd_target[275], running;
    int write_fd, read_fd;
    atomic_int rl_keystates[KEY_COUNT]; // Using atomic variables
    struct keystate vr_keystates[KEY_COUNT];
    atomic_int last_pressed; // Variable for storing the last pressed key index
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
    },
    .last_pressed = ATOMIC_VAR_INIT(-1) // Initialize variable
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
struct io_uring ring;

volatile sig_atomic_t running_flag = 1;

int main() {
    if (signal(SIGINT, sigint_handler) == SIG_ERR) {
        perror("Failed to set signal handler");
        exit(1);
    }

    if (geteuid() != 0) {
        fprintf(stderr, "This program requires sudo to access keyboard inputs\n");
        exit(1);
    }

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

    result(io_uring_queue_init(256, &ring, 0));

    while (running_flag) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (!sqe) continue;

        struct input_event ev[64];
        io_uring_prep_read(sqe, context.read_fd, ev, sizeof(ev), 0);
        io_uring_sqe_set_data(sqe, ev);

        if (io_uring_submit(&ring) < 0) continue;

        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0) continue;

        if (cqe->res < 0) {
            io_uring_cqe_seen(&ring, cqe);
            continue;
        }

        unsigned int num_events = (unsigned int)(cqe->res / sizeof(struct input_event));
        for (unsigned int i = 0; i < num_events; ++i) {
            process_event(&((struct input_event *)cqe->user_data)[i]);
        }

        io_uring_cqe_seen(&ring, cqe);
        emit_all();
    }

    result(ioctl(context.write_fd, UI_DEV_DESTROY));
    close(context.write_fd);
    close(context.read_fd);
    io_uring_queue_exit(&ring);

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t_attrs);
}

void sigint_handler(int sig) {
    (void)sig; 
    running_flag = 0; // Set running_flag to terminate the loop
}

void setup_write() {
    context.write_fd = open(context.wr_target, O_WRONLY | O_NONBLOCK);
    result(context.write_fd);

    result(ioctl(context.write_fd, UI_SET_EVBIT, EV_KEY));
    for (int i = 0; i < KEY_COUNT; i++) {
        result(ioctl(context.write_fd, UI_SET_KEYBIT, context.vr_keystates[i].which));
    }

    struct uinput_setup setup = { .name = "socd_cleaner", .id = { .bustype = BUS_USB, .vendor = 0x1234, .product = 0x5678 } };
    result(ioctl(context.write_fd, UI_DEV_SETUP, &setup));
    result(ioctl(context.write_fd, UI_DEV_CREATE));
}

void process_event(const struct input_event *ev) {
    if (ev->value == 1) { // Key down
        for (int i = 0; i < KEY_COUNT; ++i) {
            if (ev->code == context.vr_keystates[i].which) {
                atomic_store(&context.rl_keystates[i], 1);
                atomic_store(&context.last_pressed, i); // Store the index of the last pressed key
            }
        }
    } else if (ev->value == 0) { // Key up
        for (int i = 0; i < KEY_COUNT; ++i) {
            if (ev->code == context.vr_keystates[i].which) {
                atomic_store(&context.rl_keystates[i], 0);
            }
        }
    }
}

void emit(int type, int code, int value) {
    struct input_event event = { .code = code, .type = type, .value = value, .time = {0, 0} };
    result(write(context.write_fd, &event, sizeof(event))); // Use the correct file descriptor
}

void emit_all() {
    struct timespec req = {0};
    req.tv_sec = 0;
    req.tv_nsec = 16670000;  // 16.67 ms for approximately 1 frame at 60fps

    // Update state and apply SOCD cleaning
    int up_pressed = atomic_load(&context.rl_keystates[UP]);
    int down_pressed = atomic_load(&context.rl_keystates[DOWN]);
    int left_pressed = atomic_load(&context.rl_keystates[LEFT]);
    int right_pressed = atomic_load(&context.rl_keystates[RIGHT]);

    // SOCD Logic
    if (left_pressed && right_pressed) {
        // Store the last pressed key to determine which action to allow
        if (atomic_load(&context.last_pressed) == LEFT) {
            // Right is pressed, but we allow delaying the action
            right_pressed = 0; // Ensure only left is active
            emit(EV_KEY, context.vr_keystates[RIGHT].which, 0); // Release right

            // Check if up or down is pressed
            if (up_pressed || down_pressed) {
                // If either key is pressed, do not delay
                emit(EV_KEY, context.vr_keystates[LEFT].which, 1); // Keep left active
            } else {
                // Wait for 16.67 ms before sending the last pressed key
                nanosleep(&req, NULL);
                emit(EV_KEY, context.vr_keystates[LEFT].which, 1); // Keep left active
            }
        } else {  // last_pressed is RIGHT
            // Left is pressed, but we allow delaying the action
            left_pressed = 0; // Ensure only right is active
            emit(EV_KEY, context.vr_keystates[LEFT].which, 0); // Release left

            // Check if up or down is pressed
            if (up_pressed || down_pressed) {
                // If either key is pressed, do not delay
                emit(EV_KEY, context.vr_keystates[RIGHT].which, 1); // Keep right active
            } else {
                // Wait for 16.67 ms before sending the last pressed key
                nanosleep(&req, NULL);
                emit(EV_KEY, context.vr_keystates[RIGHT].which, 1); // Keep right active
            }
        }
    }

    // Handle up and down pressing
    if (up_pressed && down_pressed) {
        if (atomic_load(&context.last_pressed) == UP) {
            down_pressed = 0;  // Ensure only up is active
        } else {
            up_pressed = 0;  // Ensure only down is active
        }

        // Emit neutral for vertical keys
        emit(EV_KEY, context.vr_keystates[UP].which, 0);
        emit(EV_KEY, context.vr_keystates[DOWN].which, 0);
    }

    // Emit the current active state
    emit(EV_KEY, context.vr_keystates[UP].which, up_pressed);
    emit(EV_KEY, context.vr_keystates[DOWN].which, down_pressed);
    emit(EV_KEY, context.vr_keystates[LEFT].which, left_pressed);
    emit(EV_KEY, context.vr_keystates[RIGHT].which, right_pressed);

    emit(EV_SYN, SYN_REPORT, 0);
}

int get_keyboard(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 1;

    strcpy(context.rd_target, path);
    char *possible_devices[8];
    int j = 0, selected = 0;
    struct dirent *dir;

    while ((dir = readdir(d)) != NULL && j < 8) {
        int len = strlen(dir->d_name);
        if (len >= 10 && strncmp(dir->d_name + len - 10, "-event-kbd", 10) == 0 &&
            strncmp(dir->d_name + len - 15, "-if", 3) != 0) {
            possible_devices[j++] = dir->d_name;
        }
    }

    if (j == 0) {
        closedir(d);
        return 1;
    }

    if (j > 1) {
        selected = prompt_user(j);
    } else {
        selected = 0; // Only one device available
    }

    strcat(context.rd_target, possible_devices[selected]);
    closedir(d);
    return 0;
}

int prompt_user(int max) {
    char c;
    while (1) {
        printf("Select keyboard device (1-%d): ", max);
        fflush(stdout);
        ssize_t read_ret = read(STDIN_FILENO, &c, 1);
        if (read_ret < 0) {
            perror("Error reading from stdin");
            continue;
        }
        if (c < '1' || c >= '1' + max) continue;
        break;
    }
    return c - '1';
}
