#define _GNU_SOURCE

// MIROKI input aggregator (miroki-inputd)
//
// Merges the RG Rotate's split input hardware into one virtual gamepad:
//   - gpio-keys-gamepad: every button as a discrete GPIO (grabbed exclusively)
//   - sc27xx vibrator:   a separate ff-memless evdev node
// RetroArch and SDL only offer rumble on the joystick's own evdev node, so
// the virtual pad carries the buttons AND FF_RUMBLE, with effect uploads
// proxied through to the vibrator.
//
// The pad mirrors whatever buttons the gpio-keys device reports, AUX
// (BTN_THUMBR) included, as plain buttons for RetroArch to bind. Its
// ABS_X/ABS_Y never move: they are there because libudev-zero only tags a
// device ID_INPUT_JOYSTICK when it has EV_ABS.
//
// It also owns the system keys (gpio-keys-system, grabbed so RetroArch
// never sees them): the volume keys step softvol's "MIROKI Playback
// Volume" through tmix and keep /data/.config/audio_level current (rcS
// restores it), and with Mode held they step the backlight instead
// (/data/.config/backlight, the raw brightness, also restored by rcS and
// by ramen when its screensaver wakes); the power key and the swivel's
// hall switch put the device
// to sleep. Sleep is "mem": the EL3 monitor's SYSTEM_SUSPEND takes the SoC
// into deep sleep. The DRM driver turns the panel off and on across it; the
// backlight is still switched off around it by hand, so nothing lights up
// before the panel is back. Holding the power key
// for two seconds powers off, with a short buzz to say the hold took.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GAMEPAD_NAME  "gpio-keys-gamepad"
#define SYSTEM_NAME   "gpio-keys-system"
#define VOLUME_CTL    "MIROKI Playback Volume"
#define VOLUME_FILE   "/data/.config/audio_level"
#define VOLUME_MAX    15
#define BACKLIGHT     "/sys/class/backlight/backlight/bl_power"
#define BRIGHTNESS    "/sys/class/backlight/backlight/brightness"
#define MAX_BRIGHTNESS "/sys/class/backlight/backlight/max_brightness"
#define BRIGHTNESS_FILE "/data/.config/backlight"
#define POWEROFF_HOLD_MS 2000
#define POWEROFF_BUZZ_MS 250
#define WAKE_GRACE_MS    500
#define VIBRA_NAME    "vibrator"
#define VIRTUAL_NAME  "MIROKI Gamepad"
#define MAX_FF_SLOTS  8
#define ANALOG_MAX    32767

#define BITS_PER_LONG     (8 * sizeof(unsigned long))
#define NLONGS(n)         (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(arr, b)  (((arr)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1)

static int gamepad_fd = -1;
static int system_fd = -1;
static int vibra_fd = -1;
static int uinput_fd = -1;
static int volume = 6;
static int mode_held;                  // the gamepad's Mode button is down
// Backlight steps, percent of max_brightness: spaced wider at the top,
// where a step is hard to see, than at the bottom.
static const int bright_pct[] = { 4, 8, 13, 20, 28, 38, 50, 63, 80, 100 };
#define BRIGHT_STEPS ((int)(sizeof(bright_pct) / sizeof(bright_pct[0])))
static long long power_down_ms = -1;   // when the power key went down
static long long wake_grace_until = 0; // ignore system keys until then

// uinput effect id -> vibrator effect id (or -1)
static int ff_map[MAX_FF_SLOTS];

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

// Log to the kernel ring buffer so failures show up in dmesg; init starts
// this daemon with no useful stdout.
static void klog(const char *fmt, ...)
{
    static int kmsg_fd = -2;
    char buf[256];
    va_list ap;
    int n;

    if (kmsg_fd == -2)
        kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);

    n = snprintf(buf, sizeof(buf), "miroki-inputd: ");
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof(buf))
        n = sizeof(buf) - 1;

    if (kmsg_fd >= 0)
        write(kmsg_fd, buf, n);
    fprintf(stderr, "%s\n", buf);
}

static int find_device_by_name(const char *device_name, bool grab)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *entry;

    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL)
    {
        char path[512], name[256] = "";
        int fd;

        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (strstr(name, device_name) != NULL)
        {
            if (grab && ioctl(fd, EVIOCGRAB, 1) < 0)
                klog("failed to grab %s: %s", path, strerror(errno));
            closedir(dir);
            return fd;
        }
        close(fd);
    }

    closedir(dir);
    return -1;
}

static int create_virtual_pad(void)
{
    unsigned long keys[NLONGS(KEY_CNT)] = { 0 };
    struct uinput_user_dev uidev;
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0)
    {
        klog("cannot open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    // Mirror the gamepad's buttons
    ioctl(gamepad_fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int code = 0; code < KEY_CNT; code++)
        if (TEST_BIT(keys, code))
            ioctl(fd, UI_SET_KEYBIT, code);

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);

    if (vibra_fd >= 0)
    {
        ioctl(fd, UI_SET_EVBIT, EV_FF);
        ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
        ioctl(fd, UI_SET_FFBIT, FF_PERIODIC);
        ioctl(fd, UI_SET_FFBIT, FF_SQUARE);
        ioctl(fd, UI_SET_FFBIT, FF_TRIANGLE);
        ioctl(fd, UI_SET_FFBIT, FF_SINE);
        ioctl(fd, UI_SET_FFBIT, FF_GAIN);
    }

    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, VIRTUAL_NAME);
    uidev.id.bustype = BUS_HOST;
    uidev.id.vendor = 0x4d49;  // "MI"
    uidev.id.product = 0x524f; // "RO"
    uidev.id.version = 1;
    uidev.absmin[ABS_X] = -ANALOG_MAX;
    uidev.absmax[ABS_X] = ANALOG_MAX;
    uidev.absmin[ABS_Y] = -ANALOG_MAX;
    uidev.absmax[ABS_Y] = ANALOG_MAX;
    uidev.ff_effects_max = (vibra_fd >= 0) ? MAX_FF_SLOTS : 0;

    if (write(fd, &uidev, sizeof(uidev)) != sizeof(uidev) ||
        ioctl(fd, UI_DEV_CREATE) < 0)
    {
        klog("uinput device creation failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void emit(int fd, unsigned short type, unsigned short code, int value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    write(fd, &ev, sizeof(ev));
}

// The vibrator is a bare LDO gate: magnitude is effectively on/off. Hand
// whatever effect the client uploads to the sc27xx driver, which plays a
// rumble at max(strong, weak).
static void ff_upload(struct uinput_ff_upload *upload)
{
    struct ff_effect effect = upload->effect;

    if (upload->effect.id >= MAX_FF_SLOTS)
    {
        upload->retval = -ENOSPC;
        return;
    }

    // Reuse the previously mapped slot on re-upload
    effect.id = ff_map[upload->effect.id];

    if (ioctl(vibra_fd, EVIOCSFF, &effect) < 0)
    {
        upload->retval = -errno;
        return;
    }

    ff_map[upload->effect.id] = effect.id;
    upload->retval = 0;
}

static void ff_erase(struct uinput_ff_erase *erase)
{
    if (erase->effect_id >= MAX_FF_SLOTS || ff_map[erase->effect_id] < 0)
    {
        erase->retval = -EINVAL;
        return;
    }

    ioctl(vibra_fd, EVIOCRMFF, ff_map[erase->effect_id]);
    ff_map[erase->effect_id] = -1;
    erase->retval = 0;
}

static void handle_uinput_event(void)
{
    struct input_event ev;

    while (read(uinput_fd, &ev, sizeof(ev)) == sizeof(ev))
    {
        if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD)
        {
            struct uinput_ff_upload upload;

            memset(&upload, 0, sizeof(upload));
            upload.request_id = ev.value;
            ioctl(uinput_fd, UI_BEGIN_FF_UPLOAD, &upload);
            ff_upload(&upload);
            ioctl(uinput_fd, UI_END_FF_UPLOAD, &upload);
        }
        else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE)
        {
            struct uinput_ff_erase erase;

            memset(&erase, 0, sizeof(erase));
            erase.request_id = ev.value;
            ioctl(uinput_fd, UI_BEGIN_FF_ERASE, &erase);
            ff_erase(&erase);
            ioctl(uinput_fd, UI_END_FF_ERASE, &erase);
        }
        else if (ev.type == EV_FF)
        {
            // Play/stop request: forward with the mapped effect id
            if (ev.code < MAX_FF_SLOTS && ff_map[ev.code] >= 0)
                emit(vibra_fd, EV_FF, ff_map[ev.code], ev.value);
            else if (ev.code == FF_GAIN)
                emit(vibra_fd, EV_FF, FF_GAIN, ev.value);
        }
    }
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void write_str(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    write(fd, val, strlen(val));
    close(fd);
}

static int run(char *const argv[])
{
    pid_t pid = fork();
    int status = -1;

    if (pid == 0)
    {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, &status, 0);
    return status;
}

// ---- volume ---------------------------------------------------------------

static void volume_load(void)
{
    FILE *f = fopen(VOLUME_FILE, "r");
    int v;

    if (!f)
        return;
    if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= VOLUME_MAX)
        volume = v;
    fclose(f);
}

static void volume_step(int dir)
{
    char level[8];
    char *argv[] = { "tmix", VOLUME_CTL, level, NULL };
    FILE *f;

    volume += dir;
    if (volume < 0)
        volume = 0;
    if (volume > VOLUME_MAX)
        volume = VOLUME_MAX;
    snprintf(level, sizeof(level), "%d", volume);
    if (run(argv) != 0)
        klog("tmix '%s' %s failed", VOLUME_CTL, level);
    mkdir("/data/.config", 0755);
    f = fopen(VOLUME_FILE, "w");
    if (f)
    {
        fprintf(f, "%d\n", volume);
        fclose(f);
    }
}

// ---- backlight --------------------------------------------------------------

static int read_int(const char *path)
{
    FILE *f = fopen(path, "r");
    int v = -1;

    if (!f)
        return -1;
    if (fscanf(f, "%d", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static int bright_raw(int step, int max)
{
    int v = max * bright_pct[step] / 100;

    return v < 1 ? 1 : v;
}

// One step up or down from the step nearest the saved level (or, with
// none saved yet, the current brightness): ramen's screensaver may have
// the screen dimmed right now, and that is not where to step from.
static void brightness_step(int dir)
{
    int max = read_int(MAX_BRIGHTNESS);
    int cur = read_int(BRIGHTNESS_FILE);

    if (cur <= 0)
        cur = read_int(BRIGHTNESS);
    int step = 0, best = -1;
    char val[16];
    FILE *f;

    if (max <= 0 || cur < 0)
        return;
    for (int i = 0; i < BRIGHT_STEPS; i++)
    {
        int d = abs(bright_raw(i, max) - cur);

        if (best < 0 || d < best)
        {
            best = d;
            step = i;
        }
    }
    step += dir;
    if (step < 0)
        step = 0;
    if (step >= BRIGHT_STEPS)
        step = BRIGHT_STEPS - 1;
    snprintf(val, sizeof(val), "%d", bright_raw(step, max));
    write_str(BRIGHTNESS, val);
    mkdir("/data/.config", 0755);
    f = fopen(BRIGHTNESS_FILE, "w");
    if (f)
    {
        fprintf(f, "%s\n", val);
        fclose(f);
    }
}

// ---- sleep and power --------------------------------------------------------

static void go_to_sleep(const char *why)
{
    int fd;

    klog("sleeping (%s)", why);
    write_str(BACKLIGHT, "1");
    sync();
    fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
    if (fd < 0 || write(fd, "mem", 3) < 0)
        klog("suspend failed: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
    write_str(BACKLIGHT, "0");
    // The wake-up key's own events follow: let them pass.
    wake_grace_until = now_ms() + WAKE_GRACE_MS;
    power_down_ms = -1;
    klog("awake");
}

// A short buzz once the hold has registered, so it can't be mistaken for
// a sleep. It plays out before poweroff: the effect stops when its owner
// closes the vibrator, and init is about to kill us.
static void power_off_buzz(void)
{
    struct ff_effect effect;

    if (vibra_fd < 0)
        return;
    memset(&effect, 0, sizeof(effect));
    effect.type = FF_RUMBLE;
    effect.id = -1;
    effect.u.rumble.strong_magnitude = 0xffff;
    effect.replay.length = POWEROFF_BUZZ_MS;
    if (ioctl(vibra_fd, EVIOCSFF, &effect) < 0)
    {
        klog("power-off buzz: %s", strerror(errno));
        return;
    }
    emit(vibra_fd, EV_FF, effect.id, 1);
    usleep(POWEROFF_BUZZ_MS * 1000);
}

static void power_off(void)
{
    char *argv[] = { "poweroff", NULL };

    klog("power key held: powering off");
    power_off_buzz();
    write_str(BACKLIGHT, "1");
    run(argv);
}

static void handle_system_event(void)
{
    struct input_event ev;

    while (read(system_fd, &ev, sizeof(ev)) == sizeof(ev))
    {
        if (now_ms() < wake_grace_until)
            continue;
        if (ev.type == EV_KEY)
        {
            switch (ev.code)
            {
            case KEY_VOLUMEUP:
                if (ev.value)
                    mode_held ? brightness_step(1) : volume_step(1);
                break;
            case KEY_VOLUMEDOWN:
                if (ev.value)
                    mode_held ? brightness_step(-1) : volume_step(-1);
                break;
            case KEY_POWER:
                if (ev.value == 1)
                    power_down_ms = now_ms();
                else if (ev.value == 0 && power_down_ms >= 0)
                {
                    power_down_ms = -1;
                    go_to_sleep("power key");
                }
                break;
            default:
                break;
            }
        }
        else if (ev.type == EV_SW && ev.code == SW_LID && ev.value)
            go_to_sleep("swivel closed");
    }
}

// Buttons and sync pass straight through.
static void handle_gamepad_event(void)
{
    struct input_event ev;

    while (read(gamepad_fd, &ev, sizeof(ev)) == sizeof(ev))
    {
        if (ev.type == EV_SYN)
            emit(uinput_fd, EV_SYN, SYN_REPORT, 0);
        else if (ev.type == EV_KEY)
        {
            // Mode still goes to RetroArch (Hotkey Enable); held, it
            // turns the volume keys into brightness keys here
            if (ev.code == BTN_MODE)
                mode_held = ev.value != 0;
            emit(uinput_fd, EV_KEY, ev.code, ev.value);
        }
    }
}

int main(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    for (int i = 0; i < MAX_FF_SLOTS; i++)
        ff_map[i] = -1;

    // The gamepad must exist; the vibrator is optional (no rumble without
    // it). Wait for the gamepad rather than racing its probe.
    for (int tries = 0; tries < 100 && gamepad_fd < 0; tries++)
    {
        gamepad_fd = find_device_by_name(GAMEPAD_NAME, true);
        if (gamepad_fd < 0)
            usleep(100000);
    }
    if (gamepad_fd < 0)
    {
        klog("gamepad device '%s' not found after 10s, giving up", GAMEPAD_NAME);
        return 1;
    }

    vibra_fd = find_device_by_name(VIBRA_NAME, false);
    if (vibra_fd < 0)
        klog("vibrator '%s' not found, rumble disabled", VIBRA_NAME);

    system_fd = find_device_by_name(SYSTEM_NAME, true);
    if (system_fd < 0)
        klog("system keys '%s' not found: no volume keys or sleep", SYSTEM_NAME);
    volume_load();

    uinput_fd = create_virtual_pad();
    if (uinput_fd < 0)
        return 1;

    klog("up: virtual pad created (gamepad grabbed, rumble %s)",
         vibra_fd >= 0 ? "on" : "off");

    struct pollfd fds[3] = {
        { .fd = gamepad_fd, .events = POLLIN },
        { .fd = uinput_fd, .events = POLLIN },
        { .fd = system_fd, .events = POLLIN },
    };

    while (running)
    {
        // While the power key is down, wake up in time for the hold.
        int timeout = -1;
        if (power_down_ms >= 0)
        {
            long long left = power_down_ms + POWEROFF_HOLD_MS - now_ms();
            if (left <= 0)
            {
                power_down_ms = -1;
                power_off();
                continue;
            }
            timeout = (int)left;
        }
        if (poll(fds, system_fd >= 0 ? 3 : 2, timeout) < 0)
        {
            if (errno == EINTR)
                continue;
            klog("poll failed: %s", strerror(errno));
            break;
        }

        if (fds[0].revents & POLLIN)
            handle_gamepad_event();
        if (fds[1].revents & POLLIN)
            handle_uinput_event();
        if (system_fd >= 0 && (fds[2].revents & POLLIN))
            handle_system_event();
    }

    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    ioctl(gamepad_fd, EVIOCGRAB, 0);
    close(gamepad_fd);
    if (system_fd >= 0)
    {
        ioctl(system_fd, EVIOCGRAB, 0);
        close(system_fd);
    }
    if (vibra_fd >= 0)
        close(vibra_fd);

    return 0;
}
