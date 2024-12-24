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
#include <pthread.h> // Include for pthread mutex

#define UP     0
#define LEFT   1
#define DOWN   2
#define RIGHT  3

#define result_msg(call, fmt, ...) \
    do { if ((call) < 0) { perror(fmt); exit(1); } } while (0)

#define result(call) result_msg(call, "call failed")

struct keystate { char pressed; int which; };

// Global context structure
static struct {
    char *wr_target, rd_target[275], running;
    int write_fd, read_fd, rl_keystates[4];
    struct keystate vr_keystates[4];
    pthread_mutex_t lock; // Mutex for atomic key state updates
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
    .lock = PTHREAD_MUTEX_INITIALIZER // Initialize mutex
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

    while (context.running) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (!sqe) {
            perror("Failed to get SQE");
            continue;
        }

        struct input_event ev[64];
        io_uring_prep_read(sqe, context.read_fd, ev, sizeof(ev), 0);
        io_uring_sqe_set_data(sqe, ev); 

        if (io_uring_submit(&ring) < 0) {
            perror("Failed to submit to io_uring");
            continue;
        }

        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0) {
            perror("io_uring_wait_cqe failed");
            continue;
        }

        if (cqe->res < 0) {
            perror("Read request failed");
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
    
    t_attrs.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t_attrs);
}

void sigint_handler(int sig) {
    (void)sig; 
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
    pthread_mutex_lock(&context.lock); // Lock the mutex for atomic updates

    // Update key states based on the input event
    if (ev->value == 1) { // Key down
        for (int i = 0; i < 4; ++i) {
            if (ev->code == context.vr_keystates[i].which) {
                context.rl_keystates[i] = 1;
                context.vr_keystates[i].pressed = 1;
            }
        }
    } else if (ev->value == 0) { // Key up
        for (int i = 0; i < 4; ++i) {
            if (ev->code == context.vr_keystates[i].which) {
                context.rl_keystates[i] = 0;
                context.vr_keystates[i].pressed = 0;
            }
        }
    }

    // SOCD Clean Up with prioritization
    int left_pressed = context.rl_keystates[LEFT];
    int right_pressed = context.rl_keystates[RIGHT];
    int up_pressed = context.rl_keystates[UP];
    int down_pressed = context.rl_keystates[DOWN];

    // Handle left and right movement with priority
    if (left_pressed && right_pressed) {
        // Prioritize last pressed key
        if (ev->value == 1 && (ev->code == context.vr_keystates[LEFT].which || ev->code == context.vr_keystates[RIGHT].which)) {
            context.vr_keystates[LEFT].pressed = (ev->code == context.vr_keystates[LEFT].which);
            context.vr_keystates[RIGHT].pressed = (ev->code == context.vr_keystates[RIGHT].which);
        } else {
            context.vr_keystates[LEFT].pressed = context.vr_keystates[LEFT].pressed;
            context.vr_keystates[RIGHT].pressed = context.vr_keystates[RIGHT].pressed;
        }
    } else {
        context.vr_keystates[LEFT].pressed = left_pressed;
        context.vr_keystates[RIGHT].pressed = right_pressed;
    }

    // Handle up and down movement with similar priority
    if (up_pressed && down_pressed) {
        // Prioritize last pressed key
        if (ev->value == 1 && (ev->code == context.vr_keystates[UP].which || ev->code == context.vr_keystates[DOWN].which)) {
            context.vr_keystates[UP].pressed = (ev->code == context.vr_keystates[UP].which);
            context.vr_keystates[DOWN].pressed = (ev->code == context.vr_keystates[DOWN].which);
        } else {
            context.vr_keystates[UP].pressed = context.vr_keystates[UP].pressed;
            context.vr_keystates[DOWN].pressed = context.vr_keystates[DOWN].pressed;
        }
    } else {
        context.vr_keystates[UP].pressed = up_pressed;
        context.vr_keystates[DOWN].pressed = down_pressed;
    }

    pthread_mutex_unlock(&context.lock); // Unlock the mutex
}

void emit(int type, int code, int value) {
    struct input_event event = { .code = code, .type = type, .value = value, .time = {0, 0} };
    result(write(context.write_fd, &event, sizeof(event)));
}

void emit_all() {
    pthread_mutex_lock(&context.lock); // Lock the mutex before reading key states

    for (int i = 0; i < 4; ++i) {
        emit(EV_KEY, context.vr_keystates[i].which, context.vr_keystates[i].pressed);
    }
    
    emit(EV_SYN, SYN_REPORT, 0);
    
    pthread_mutex_unlock(&context.lock); // Unlock the mutex after emitting
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
