/*
 * ramen: MIROKI's RetroArch menu driver.
 *
 * The design is menu-design.md alongside this file: a glass tube
 * on a quarter circle around the screen's pivot corner, with each system a
 * sphere riding in it. This file is copied into RetroArch's menu/drivers/ by
 * tools/build_retroarch.sh, which also applies system/ramen/retroarch-hook.patch
 * to register it; it is not part of the RetroArch tree.
 *
 * It only targets the gl2 video driver (GLES 3.2 through libmali). The whole
 * scene is drawn by one fullscreen fragment shader fed with the sphere
 * positions, so there are no image assets; RetroArch's font driver renders the
 * labels on top.
 *
 * Geometry is in world space: the pixels of the system view, pivot at the
 * screen's bottom-right. The camera pans by one screen width into the game
 * view and zooms out around the pivot, which the shader applies to every
 * pixel, so the two views are one scene seen from two places.
 *
 * Status: system carousel (boot spin, detent spring, labels), the launch
 * sequence and its reverse, the game view with capsules from the ROM
 * directories, and launching a game. No monitor or sound yet.
 */

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <compat/strl.h>
#include <dynamic/dylib.h>
#include <features/features_cpu.h>
#include <file/config_file.h>
#include <file/file_path.h>
#include <formats/image.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <retro_miscellaneous.h>
#include <retro_timers.h>
#include <rthreads/rthreads.h>
#include <string/stdstring.h>

#include "../../config.h"
#include "../../content.h"
#include "../../gfx/common/gl2_common.h"
#include "../../gfx/font_driver.h"
#include "../../gfx/gfx_display.h"
#include "../../gfx/video_driver.h"
#include "../../runtime_file.h"
#include "../../tasks/task_content.h"
#include "../../verbosity.h"
#include "../menu_driver.h"
#include "../../retroarch.h"
#include "../../command.h"
#include "../../audio/audio_driver.h"
#include "../../core_option_manager.h"
#include "../../runloop.h"
#include "../../file_path_special.h"
#include "../../paths.h"
#include "../../input/input_driver.h"
#include "../../input/input_remapping.h"

#define RAMEN_FONT_PATH   "/usr/share/miroki/fonts/Audiowide-Regular.ttf"
#define RAMEN_STATE_PATH  "/data/.config/ramen"
#define RAMEN_CORE_DIR    "/usr/lib/libretro"

/* User content: bios, cores and roms at the top of /data and, when a card
 * is in, the SD card. /data comes first wherever both have something. */
#define RAMEN_SD_DIR      "/sd"
static const char *const ramen_roots[] = { "/data", RAMEN_SD_DIR };
#define RAMEN_ROOTS       2

/* RetroArch takes one system directory: links to the entries of every
 * root's bios directory (etc/retroarch.cfg points here). */
#define RAMEN_BIOS_LINKS  "/run/miroki/bios"

/* Carousel: slots 40 degrees apart cycling through the systems, A B C A B C
 * for as far as the wheel turns, so any number of systems repeats evenly.
 * The selected slot sits at 45 degrees (the screen diagonal), its neighbours
 * at 5 and 85, half clipped by the bottom and right edges. */
#define RAMEN_SLOT_DEG    40.0f
#define RAMEN_SELECT_DEG  45.0f
#define RAMEN_BUILTINS    3
#define RAMEN_SYSTEMS_MAX 32
#define RAMEN_SYS_DIRS    3

/* Game view: the circle's upper-right quadrant, 90..180 degrees. The system
 * sphere parks on its diagonal and the games are smaller spheres queued
 * clockwise of it: the selected one just past the sphere, the ones before
 * it trailing toward the bottom edge, the ones after it still inside the
 * sphere. Stepping forward turns the queue clockwise. The slot spacing
 * depends on how many games there are, so a long list stacks up. */
#define RAMEN_PARK_DEG    112.0f
#define RAMEN_GAME_DEG    135.0f   /* the quadrant's diagonal */
#define RAMEN_GAME_EMERGE 14.0f    /* past the sphere's edge, fade-in start */
#define RAMEN_GAME_FADE   10.0f    /* degrees of fade at either end */
#define RAMEN_GAME_END    198.0f   /* past the bottom edge */
#define RAMEN_GAME_SLOT_MIN 16.0f
#define RAMEN_GAME_SLOT_MAX 18.0f
#define RAMEN_GAME_SCALE  0.75f
#define RAMEN_GAMES_MAX   10    /* spheres drawn at once */
#define RAMEN_MAX_SPHERES 16

/* Box art, shown inside the game spheres: boxart/<game>.png or .jpg in
 * the game's own directory, centre-cropped square and resampled into one layer
 * of a texture array, so the shader can pick a sphere's art by index. The
 * cache holds the spheres around the selection and RAMEN_ART_AHEAD more
 * past either end of the ring, replacing the least recently used. A
 * worker thread does the decoding, so the game view never waits on it:
 * the render thread hands it paths and uploads what comes back. */
#define RAMEN_ART_DIR     "boxart"
#define RAMEN_ART_SIZE    256
#define RAMEN_ART_LAYERS  24
#define RAMEN_ART_AHEAD   4
#define RAMEN_ART_JOBS    32

/* Tube radius and sphere radius as a fraction of the screen side. */
#define RAMEN_TUBE_R      0.65f
#define RAMEN_SPHERE_R    0.19f

/* Detent spring: natural frequency and damping ratio (design: 0.6-0.8). */
#define RAMEN_SPRING_HZ   2.235f
#define RAMEN_SPRING_ZETA 0.315f

/* Boot spin: exponential friction, wheel starts this far back. Under
 * friction alone it would coast to RAMEN_BOOT_OVERRUN degrees past the
 * target; the spring takes over before that, at the point where it brakes
 * rather than pulls, so the wheel never speeds up into the detent. */
#define RAMEN_BOOT_TURNS  1.875f
#define RAMEN_BOOT_FRICTION 2.65f
#define RAMEN_BOOT_OVERRUN  60.0f

/* Camera: a spring trailing the sphere, and the game view's zoom. */
#define RAMEN_CAM_HZ      1.6f
#define RAMEN_CAM_ZETA    0.9f
#define RAMEN_GAME_ZOOM   0.85f

/* The launch sequence's timeline (seconds): confirm jiggle and clear-out,
 * anticipation, then the run along the tube. */
#define RAMEN_SEQ_CLEAR   0.25f
#define RAMEN_SEQ_ANTIC   0.20f
#define RAMEN_SEQ_RUN     0.65f
#define RAMEN_SEQ_TOTAL   (RAMEN_SEQ_CLEAR + RAMEN_SEQ_ANTIC + RAMEN_SEQ_RUN)
#define RAMEN_FADE_S      0.45f
/* Game launch: the camera dives into the monitor's glass until the bezel
 * leaves the frame, and the picture fades out partway through the dive.
 * Coming back from a game plays it in reverse: fade in on the glass, then
 * pull back out to the game view. */
#define RAMEN_LAUNCH_S    0.5f
#define RAMEN_LAUNCH_ZOOM 2.5f
#define RAMEN_LAUNCH_FADE 0.33f    /* the dive's progress when the fade starts */
/* Once the sphere has parked, the games come out of it one after the
 * other: each bursts from the same point inside the sphere to its own
 * slot, the selected one first and the trailing ones staggered behind it,
 * so the list fills from the sphere down to the bottom edge. On the way
 * back they fall out of the bottom of the tube, accelerating, and nothing
 * follows them. */
#define RAMEN_INTRO_START   (RAMEN_PARK_DEG - 15.0f)
#define RAMEN_INTRO_BURST   0.35f
#define RAMEN_INTRO_STAGGER 0.08f
#define RAMEN_OUTRO_S       0.45f
#define RAMEN_OUTRO_DEG     90.0f

/* The monitor, in world space relative to the pivot and in units of S: it
 * sits in the upper right of the game view, clear of the tube's quadrant,
 * and pans in with the camera. Power-on is the CRT line opening out;
 * power-off collapses it again. */
#define RAMEN_MON_X       0.48f
#define RAMEN_MON_Y       1.13f    /* top edge, measured up from the pivot */
#define RAMEN_MON_W       0.64f
#define RAMEN_MON_H       0.49f
#define RAMEN_MON_INSET   0.075f   /* bezel width as a fraction of MON_W */
#define RAMEN_MON_ON_S    0.40f
#define RAMEN_MON_OFF_S   0.25f

/* The in-game menu (Sequence 6): the paused frame, dimmed and desaturated
 * under a green panel with scanlines, opened with the CRT power-on line
 * and closed with the power-off collapse (to a line, then a dot). A page
 * switch degausses the picture: a wobble and a colour purity swirl that
 * decay over the effect, with the new page drawn only once it settles. */
#define RAMEN_IG_OPEN_S    0.30f
#define RAMEN_IG_VCLOSE_S  0.16f
#define RAMEN_IG_HCLOSE_S  0.14f
#define RAMEN_DEGAUSS_S    0.55f
#define RAMEN_DEGAUSS_SHOW 0.30f   /* the page appears this far in */
#define RAMEN_IG_BINDS     (RARCH_FIRST_CUSTOM_BIND + 8)
/* A selected label that doesn't fit scrolls to its end and back, holding
 * this long at either end, at this many ems a second. */
#define RAMEN_IG_SCROLL_HOLD 1.2f
#define RAMEN_IG_SCROLL_EMS  2.5f

enum ramen_ig_page
{
   IG_TOP = 0,
   IG_OPTIONS,
   IG_REMAPS,
   IG_PAGES
};

enum ramen_ig_cmd
{
   IG_CMD_NONE = 0,
   IG_CMD_RESUME,
   IG_CMD_SAVE,
   IG_CMD_LOAD,
   IG_CMD_EXIT
};

enum
{
   IG_TOP_RESUME = 0,
   IG_TOP_SAVE,
   IG_TOP_LOAD,
   IG_TOP_SETTINGS,
   IG_TOP_EXIT,
   IG_TOP_COUNT
};

/* Sounds: short synthesised cues, loaded into the mixer's system slots as
 * WAVs built in memory. The audio driver is torn down with every core
 * load and takes the mixer's streams with it, so they are reloaded
 * whenever a slot turns up empty. */
#define RAMEN_SND_RATE    44100
#define RAMEN_SND_VOL     0.55f
enum ramen_sound
{
   SND_TICK = AUDIO_MIXER_SYSTEM_SLOT_OK,
   SND_TICK1,            /* the tick rotates over three slots, so that */
   SND_TICK2,            /* a fast run of steps does not drop any */
   SND_CONFIRM,
   SND_BACK,
   SND_THUNK,
   SND_POWER,
   SND_OFF,
   SND_END
};

/* Storage Mode from the carousel: Start unmounts the SD card so it can be
 * swapped and hands /data to the host over USB; a host eject (or Start
 * again) takes /data back and mounts whatever card is in. /data and the
 * card slot are polled this often. */
#define RAMEN_USB_POLL_S  0.5f

/* Idle: once nothing is moving and there has been no input for a while,
 * the menu drops to about 20 fps (each pass stretched to 50 ms, three
 * vsyncs). Rendering the scene at 60 fps over a still picture cost about
 * 69 mA of the ~197 mA the device drew at the menu. RetroArch's own input
 * clock is used, so no press is swallowed on the way out. */
#define RAMEN_IDLE_AFTER_US  3000000
#define RAMEN_IDLE_FRAME_US  50000

/* RetroArch's menu screensaver (menu_screensaver_timeout) dims the
 * backlight to this share of its maximum and paces the menu at 5 fps;
 * the first press, which RetroArch keeps for itself, brings both back. */
#define RAMEN_BACKLIGHT_DIR  "/sys/class/backlight/backlight"
#define RAMEN_BACKLIGHT_LEVEL "/data/.config/backlight"   /* miroki-inputd */
#define RAMEN_SAVER_DIM_PCT  10
#define RAMEN_SAVER_FRAME_US 200000

/* Battery and volume, as two glass vials with metal caps: stacked in the
 * carousel's top left, side by side along the bottom of the in-game menu.
 * The level comes from the fuel gauge, charging from the charger, the
 * volume from the level miroki-inputd keeps on data (0-15). All are polled
 * only while the vials are up. Sizes are fractions of S: the glass's
 * half-width and half-height, and a cap's half-height. */
#define RAMEN_FGU_PATH      "/sys/class/power_supply/sc27xx-fgu"
#define RAMEN_CHARGER_PATH  "/sys/class/power_supply/aw32257-0"
#define RAMEN_VOLUME_PATH   "/data/.config/audio_level"
#define RAMEN_VOLUME_MAX    15
#define RAMEN_STATUS_POLL_S 0.25f
#define RAMEN_HUD_S         0.20f
#define RAMEN_VIAL_HW       0.030f
#define RAMEN_VIAL_HH       0.070f
#define RAMEN_VIAL_CAP      0.014f
#define RAMEN_HUD_TEXT      0.84f

/* Pivot corner in screen space (origin top-left, y down): the design is
 * written for the open orientation, pivot at the bottom-right. Switch these
 * if the panel comes up with the hinge elsewhere. */
#define RAMEN_PIVOT_X     1.0f
#define RAMEN_PIVOT_Y     1.0f

/* Touch: the panel is read straight from evdev (RetroArch's menu pointer
 * never gets a position out of its udev touch code). A finger on the tube
 * turns the wheel it is on by the angle it sweeps about the pivot; letting
 * go flings it by the last velocity and the detent spring takes over. A
 * finger that sweeps less than TAP_DEG taps: the selected sphere confirms,
 * a neighbour is pulled in. Too close to the pivot the angle is noise. */
#define RAMEN_TOUCH_TAP_DEG   2.5f
#define RAMEN_TOUCH_FLING_S   0.12f   /* the systems' look-ahead */
#define RAMEN_TOUCH_MAX_STEPS 3       /* the systems' wheel: it repeats */
/* The games' ring is long, so a flick there coasts under friction
 * instead, ticking past games until it is slow enough for the spring. */
#define RAMEN_GAME_FRICTION   3.0f    /* 1/s */
#define RAMEN_GAME_COAST_MIN  70.0f   /* deg/s: below this the spring */
#define RAMEN_GAME_COAST_MAX  1500.0f /* deg/s */
#define RAMEN_TOUCH_MIN_R     0.2f    /* of S, from the pivot */
#define RAMEN_TOUCH_BAND      1.5f    /* of a sphere radius, off the tube */

/* Motion: the accelerometer through its IIO buffer. The spheres have
 * inertia: a jolt of the device leaves them behind, so the part of the
 * acceleration along the tube at the selected slot kicks the wheel the
 * other way, and the spring brings it back. Gravity is a slow average and
 * does not count (a tilt must not pull the wheel off its detent). The
 * kick is capped well under half a slot so it can never change the
 * selection. Which device axis is which way on screen is set here; the
 * hub's mount matrix is the identity. */
#define RAMEN_IMU_NAME      "sprd-shub-accel"
#define RAMEN_IMU_HZ        50
#define RAMEN_IMU_AXIS_X    0       /* device axis along the screen's x */
#define RAMEN_IMU_AXIS_Y    1       /* device axis along the screen's y */
#define RAMEN_IMU_SIGN_X    1.0f    /* +1: that axis grows to the right */
#define RAMEN_IMU_SIGN_Y    1.0f    /* +1: that axis grows upward */
#define RAMEN_IMU_SCALE     (1.0f / 256.0f)
#define RAMEN_IMU_G_TAU     0.8f    /* seconds, the gravity average */
#define RAMEN_IMU_DEAD      0.35f   /* m/s^2 of noise to ignore */
#define RAMEN_IMU_KICK      260.0f  /* deg/s^2 per m/s^2 */
#define RAMEN_IMU_VMAX      220.0f  /* deg/s, the systems' wheel */
#define RAMEN_IMU_GVMAX     110.0f  /* deg/s, the games' */

static const struct
{
   const char *name;
   const char *dir;      /* under each root's roms */
   const char *core;     /* under RAMEN_CORE_DIR */
   const char *exts;     /* dir_list_new() filter */
   float rgb[3];
} ramen_builtins[RAMEN_BUILTINS] = {
   { "N64",    "n64",    "mupen64plus_pow2_libretro.so",
     "n64|v64|z64|bin|u1|ndd",
     { 0.22f, 0.60f, 0.95f } },
   { "PS1",    "psx",    "pcsx_rearmed_libretro.so",
     "cue|chd|pbp|m3u|img|iso|bin|mdf|toc|cbn|exe",
     { 0.55f, 0.55f, 0.60f } },
   { "Saturn", "saturn", "yabasanshiro_libretro.so",
     "cue|chd|iso|mds|ccd",
     { 0.95f, 0.45f, 0.20f } },
};

/* The carousel's systems: the built-ins, then the user's cores from each
 * root's cores directory (see ramen_scan_cores). dirs[0] is the system's
 * key, saved as the last played. */
typedef struct
{
   char name[64];
   char dirs[RAMEN_SYS_DIRS][64];   /* under each root's roms */
   int  ndirs;
   char core[PATH_MAX_LENGTH];
   char exts[512];
   float rgb[3];
   bool custom;
} ramen_system_t;

enum ramen_state
{
   RAMEN_SYSTEM = 0,     /* the carousel */
   RAMEN_LAUNCHING,      /* sequence 3, seq runs 0 -> 1 */
   RAMEN_GAME,           /* the game view */
   RAMEN_RETURNING,      /* games fall out, then seq runs 1 -> 0 */
   RAMEN_STARTING,       /* diving into the glass, then the core loads */
   RAMEN_INGAME,         /* the in-game menu over the paused frame */
   RAMEN_USB             /* Storage Mode: /data exported, the card out */
};

typedef struct ramen_handle
{
   enum ramen_state state;
   retro_time_t last_us;
   float dt;             /* this frame's step, seconds */

   /* Wheel physics. wheel is the rotation of slot 0 in degrees; slot k sits
    * at wheel + k * 40. Unwrapped, so it just grows. */
   float wheel;
   float wheel_vel;
   float wheel_target;
   bool boot_spin;       /* friction phase before the spring takes over */
   bool settled;

   /* Confirm jiggle (0 = idle), squash-and-stretch along the tangent. */
   float jiggle;

   /* Label surfacing, 0..1. */
   float label;

   /* The launch sequence: seq in 0..1 over RAMEN_SEQ_TOTAL seconds, the
    * system being launched, and the camera (pan in world px, zoom). */
   float seq;
   int   system;
   float cam;
   float cam_vel;
   float zoom;
   float fade;
   float launch;         /* the dive into the glass, 0..1 */
   bool  snap_cam;       /* park the camera on the next frame */

   /* The in-game menu: page and cursors, the raster (open vertically on
    * the way in, collapse vertically then horizontally on the way out),
    * the command to run once collapsed, and the degauss clock. */
   enum ramen_ig_page ig_page;
   int   ig_sel[IG_PAGES];
   float ig_v;
   float ig_h;
   bool  ig_closing;
   enum ramen_ig_cmd ig_cmd;
   float ig_degauss;     /* seconds since the page switch, or -1 */
   bool  ig_remaps_dirty;
   bool  ig_remaps_game;  /* save them for this game alone */
   /* A label too long for its row scrolls while selected: which row the
    * clock belongs to, and the clock. */
   int   ig_scroll_page;
   int   ig_scroll_row;
   float ig_scroll_t;

   /* Boot spin ticks, Storage Mode, and a notice line in the carousel. */
   int   boot_slot;
   int   usb_req;        /* 1 = enter, 2 = take /data back, 3 = it is back */
   float usb_poll;
   bool  sd_present;     /* a card in the slot, polled in Storage Mode */
   char  notice[128];
   float notice_t;

   /* The vials: battery percent (-1 unknown) and whether it is charging,
    * the volume level (-1 unknown), the poll countdown, their visibility
    * 0..1, and a clock for the bubbles. */
   int   bat_pct;
   bool  bat_charging;
   int   vol;
   float status_poll;
   float hud;
   float clock;

   /* When the last render pass ended, for the idle frame pacing. */
   retro_time_t idle_mark;

   /* The screensaver is up: the brightness to go back to. */
   bool  saver;
   int   saver_brightness;

   /* The systems on the carousel, built-ins first. */
   ramen_system_t sys[RAMEN_SYSTEMS_MAX];
   int   nsys;

   /* The game view: one list per system, loaded on first entry; the
    * game wheel is the capsule queue's rotation, same spring as the big
    * one. */
   struct string_list *games[RAMEN_SYSTEMS_MAX];
   bool games_loaded[RAMEN_SYSTEMS_MAX];
   int   game;           /* selected index */
   float gslot;          /* degrees between games, from the count */
   float gwheel;         /* degrees, unwrapped; grows stepping forward */
   float gwheel_vel;
   float gwheel_target;
   bool  gsettled;
   /* The monitor's power, 0..1, and the metadata it shows for the
    * selected game, refreshed when the selection changes. */
   float mon;
   int   meta_sys;
   int   meta_game;
   char  meta_runtime[64];
   char  meta_last[64];

   /* Box art cache: which game each layer holds, whether a file was
    * found (or is still being decoded), and when it was last drawn. */
   GLuint art_tex;
   struct
   {
      int sys, game;
      bool present;
      bool pending;
      unsigned stamp;
   } art[RAMEN_ART_LAYERS];
   unsigned art_stamp;

   /* The decoder thread and its jobs. A job goes QUEUED by the render
    * thread, BUSY while the worker decodes, DONE for the render thread to
    * upload and free. Everything but the decode runs under art_lock. A
    * job carries the generation it was made in, so a rescan can drop
    * whatever was in flight. */
   sthread_t *art_thread;
   slock_t   *art_lock;
   scond_t   *art_cond;
   bool       art_quit;
   unsigned   art_gen;
   unsigned   art_seq;
   struct
   {
      enum { ART_FREE, ART_QUEUED, ART_BUSY, ART_DONE } state;
      unsigned seq;      /* queue order */
      unsigned gen;
      int sys, game;
      char path[PATH_MAX_LENGTH];
      uint8_t *buf;      /* RGBA, or NULL for no art */
   } art_jobs[RAMEN_ART_JOBS];

   /* The touch panel: its evdev node and ranges, the finger as reported
    * (committed on each SYN), and the drag it drives: 0 none, 1 the
    * systems' wheel, 2 the games'; the wheel when it began, the angle
    * swept since (unwrapped), the last angle seen, the velocity, and
    * whether it has moved past a tap. */
   int   touch_fd;
   int   touch_max_x;
   int   touch_max_y;
   int   touch_raw_x;
   int   touch_raw_y;
   bool  touch_raw_down;
   bool  touch_down;
   float touch_x;        /* screen px */
   float touch_y;
   int   drag;
   float drag_w0;
   float drag_acc;
   float drag_last;
   float drag_vel;
   bool  drag_moved;
   bool  gcoast;         /* the games' ring is coasting after a flick */

   /* The camera as last drawn: the pivot on screen, the zoom, the pivot
    * and S in world px, and the screen size. Touches map through it. */
   float cam_ps[2];
   float cam_zoom;
   float cam_pivot[2];
   float cam_s;
   unsigned cam_w;
   unsigned cam_h;

   /* The accelerometer: its buffer device and sysfs dir, whether the
    * stream is on, and the gravity average. */
   int   imu_fd;
   char  imu_sysfs[96];
   bool  imu_on;
   bool  imu_have_g;
   float imu_g[3];

   float gintro;         /* seconds since the games began bursting out */
   float gintro_len;     /* when the last visible one has settled */
   float goutro;         /* seconds since they began falling out */
   bool  started;        /* RAMEN_STARTING: the load task was pushed */

   /* GL */
   bool gl_ready;
   GLuint prog;
   GLuint vao;
   GLint u_size;
   GLint u_pivot;
   GLint u_cam;
   GLint u_zoom;
   GLint u_fade;
   GLint u_radius;
   GLint u_sphere_r;
   GLint u_count;
   GLint u_spheres;
   GLint u_colors;
   GLint u_alpha;
   GLint u_mon;
   GLint u_mon_rect;
   GLint u_art_tex;
   GLint u_art;
   GLint u_game;
   GLint u_game_rect;
   GLint u_game_uv;
   GLint u_game_flip;
   GLint u_ig;
   GLint u_ig_open;
   GLint u_degauss;
   GLint u_vial;
   GLint u_vial_rgb;
   GLint u_vial_dim;
   GLint u_hud;
   GLint u_phosphor;
   GLint u_time;

   font_data_t *font;
   float font_size;
} ramen_handle_t;

/* ---- shaders --------------------------------------------------------- */

static const char *ramen_vert =
   "#version 300 es\n"
   "out vec2 v_px;\n"
   "uniform vec2 u_size;\n"
   "void main() {\n"
   "  vec2 p = vec2((gl_VertexID & 1) * 4 - 1, (gl_VertexID & 2) * 2 - 1);\n"
   "  gl_Position = vec4(p, 0.0, 1.0);\n"
   /* screen space, y down, in pixels */
   "  v_px = vec2(p.x + 1.0, 1.0 - p.y) * 0.5 * u_size;\n"
   "}\n";

/* Each sphere: x = angle (rad), y = scale, z = tangential stretch, w = dim.
 * Angle 0 points left along the bottom edge from the pivot, pi/2 up the
 * right edge, pi right along the bottom edge; increasing clockwise on
 * screen. The camera maps screen pixels to world pixels: a pan of u_cam
 * along x and a zoom about the pivot. */
static const char *ramen_frag =
   "#version 300 es\n"
   "precision highp float;\n"
   "in vec2 v_px;\n"
   "out vec4 frag;\n"
   "uniform vec2 u_size;\n"
   "uniform vec2 u_pivot;\n"
   "uniform vec2 u_cam;\n"
   "uniform float u_zoom;\n"
   "uniform float u_fade;\n"
   "uniform float u_radius;\n"
   "uniform float u_sphere_r;\n"
   "uniform int u_count;\n"
   "uniform vec4 u_spheres[16];\n"
   "uniform vec3 u_colors[16];\n"
   "uniform float u_alpha[16];\n"
   "uniform float u_mon;\n"
   "uniform vec4 u_mon_rect;\n"
   "uniform highp sampler2DArray u_art_tex;\n"
   "uniform float u_art[16];\n"
   "uniform sampler2D u_game;\n"
   "uniform vec4 u_game_rect;\n"
   "uniform vec2 u_game_uv;\n"
   "uniform float u_game_flip;\n"
   "uniform float u_ig;\n"
   "uniform vec2 u_ig_open;\n"
   "uniform float u_degauss;\n"
   "uniform vec4 u_vial[2];\n"
   "uniform vec3 u_vial_rgb[2];\n"
   "uniform vec3 u_vial_dim;\n"
   "uniform float u_hud;\n"
   "uniform float u_phosphor;\n"
   "uniform float u_time;\n"
   "\n"
   /* signed distance to a rounded rectangle: half-size h, corner radius r */
   "float rrect(vec2 p, vec2 h, float r) {\n"
   "  vec2 q = abs(p) - h + r;\n"
   "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
   "}\n"
   "\n"
   /* A vial in screen space: v = centre, liquid level 0..1, bubbles (1 while
    * charging). The glass is a cylinder seen side on, so its walls brighten
    * where they are seen edge on; the liquid climbs them in a meniscus. A
    * short metal cylinder caps either end. cov collects the coverage. */
   "vec3 vial(vec3 col, vec2 p, vec4 v, vec3 liq, inout float cov) {\n"
   "  float hw = u_vial_dim.x;\n"
   "  float hh = u_vial_dim.y;\n"
   "  float ch = u_vial_dim.z;\n"
   "  float cw = hw * 1.22;\n"
   "  vec2 q = p - v.xy;\n"
   "  if (abs(q.x) > cw + 2.0 || abs(q.y) > hh + 2.0 * ch + 2.0) return col;\n"
   "  float body = 1.0 - smoothstep(-0.75, 0.75, rrect(q, vec2(hw, hh), hw * 0.3));\n"
   "  float xn = clamp(q.x / hw, -1.0, 1.0);\n"
   "  float xz = sqrt(max(0.0, 1.0 - xn * xn));\n"
   "  vec3 glass = vec3(0.55, 0.70, 0.85);\n"
   "  vec3 b = mix(col, glass * 0.30, 0.25);\n"
   "  float top = hh - 2.0 * hh * v.z;\n"
   "  float men = top - hw * 0.35 * xn * xn;\n"
   "  float lq = (1.0 - smoothstep(-0.75, 0.75, rrect(q, vec2(hw - 1.5, hh - 1.0), hw * 0.25)))\n"
   "           * smoothstep(men - 0.75, men + 0.75, q.y) * step(0.001, v.z);\n"
   "  vec3 lc = liq * (0.50 + 0.50 * xz);\n"
   "  lc += liq * 0.45 * exp(-abs(q.y - men) / 0.9);\n"
   "  if (v.w > 0.5) {\n"
   "    for (int i = 0; i < 3; i++) {\n"
   "      float fi = float(i);\n"
   "      float t = fract(u_time * (0.35 + 0.12 * fi) + fi * 0.37);\n"
   "      vec2 bc = vec2(hw * (-0.45 + 0.45 * fi), mix(hh - 2.0, men + 2.0, t));\n"
   "      float br = hw * (0.14 + 0.04 * fi);\n"
   "      float bd = length(q - bc);\n"
   "      lc = mix(lc, vec3(0.92), (1.0 - smoothstep(br - 0.6, br + 0.6, bd)) * 0.45);\n"
   "    }\n"
   "  }\n"
   "  b = mix(b, lc, lq * 0.92);\n"
   "  b += glass * (1.0 - xz) * 0.45;\n"
   "  b += vec3(1.0) * exp(-pow((xn + 0.45) / 0.14, 2.0)) * 0.30;\n"
   "  col = mix(col, b, body);\n"
   "  cov = max(cov, body);\n"
   "  for (int k = 0; k < 2; k++) {\n"
   "    vec2 cq = q - vec2(0.0, (k == 0 ? -1.0 : 1.0) * (hh + ch * 0.85));\n"
   "    float cap = 1.0 - smoothstep(-0.75, 0.75, rrect(cq, vec2(cw, ch), ch * 0.35));\n"
   "    float cx = clamp(cq.x / cw, -1.0, 1.0);\n"
   "    vec3 m = vec3(0.50, 0.52, 0.55) * (0.30 + 0.55 * sqrt(max(0.0, 1.0 - cx * cx)));\n"
   "    m += vec3(1.0) * exp(-pow((cx + 0.35) / 0.18, 2.0)) * 0.35;\n"
   /* a groove round the middle */
   "    m *= 1.0 - 0.45 * exp(-cq.y * cq.y);\n"
   "    col = mix(col, m, cap);\n"
   "    cov = max(cov, cap);\n"
   "  }\n"
   "  return col;\n"
   "}\n"
   "\n"
   /* both vials, faded by u_hud; on the in-game menu they glow in the
    * phosphor's green like the rest of the page */
   "vec3 hud(vec3 col, vec2 p) {\n"
   "  if (u_hud <= 0.0) return col;\n"
   "  float cov = 0.0;\n"
   "  vec3 v = vial(col, p, u_vial[0], u_vial_rgb[0], cov);\n"
   "  v = vial(v, p, u_vial[1], u_vial_rgb[1], cov);\n"
   "  float lum = dot(v, vec3(0.299, 0.587, 0.114));\n"
   "  vec3 ph = vec3(0.012, 0.060, 0.032) + lum * vec3(0.55, 1.0, 0.70) * 1.3;\n"
   "  v = mix(v, ph, u_phosphor * cov);\n"
   "  return mix(col, v, u_hud);\n"
   "}\n"
   "\n"
   "void main() {\n"
   "  float S = min(u_size.x, u_size.y);\n"
   /* the in-game menu: the paused frame as seen on the monitor */
   "  if (u_ig > 0.5) {\n"
   "    vec2 c = v_px / u_size * 2.0 - 1.0;\n"
   "    vec2 p = v_px;\n"
   "    float dg = u_degauss;\n"
   "    float env = dg >= 0.0 ? exp(-dg * 6.0) : 0.0;\n"
   /* degauss: the geometry wobbles, hardest at the start */
   "    p.x += env * 0.035 * S * sin(c.y * 7.0 - dg * 38.0);\n"
   "    p.y += env * 0.015 * S * sin(c.x * 5.0 + dg * 31.0);\n"
   "    vec2 guv = (p - u_game_rect.xy) / u_game_rect.zw;\n"
   "    vec3 g = vec3(0.0);\n"
   "    if (guv.x >= 0.0 && guv.x <= 1.0 && guv.y >= 0.0 && guv.y <= 1.0)\n"
   "      g = texture(u_game, vec2(guv.x, u_game_flip > 0.5 ? 1.0 - guv.y : guv.y) * u_game_uv).rgb;\n"
   "    vec3 graw = g;\n"
   /* dim and desaturate first, then the green */
   "    float lum = dot(g, vec3(0.299, 0.587, 0.114));\n"
   "    g = mix(g, vec3(lum), 0.8) * 0.38;\n"
   "    vec3 col = vec3(0.012, 0.060, 0.032) + g * vec3(0.55, 1.0, 0.70);\n"
   "    col *= 0.82 + 0.18 * sin(v_px.y * 1.57);\n"
   "    col *= 1.0 - 0.18 * dot(c, c);\n"
   /* colour purity swirl: the beams land off the mask while magnetised */
   "    if (dg >= 0.0) {\n"
   "      float sw = exp(-dg * 5.0);\n"
   "      float ang = atan(c.y, c.x) + dg * 7.0 + length(c) * 5.0;\n"
   "      col += sw * 0.09 * (0.4 + lum) * vec3(sin(ang), sin(ang + 2.094), sin(ang + 4.189));\n"
   "    }\n"
   /* the raster: a line opening out, or collapsing to a line and a dot */
   "    float hx = u_ig_open.x;\n"
   "    float vy = u_ig_open.y;\n"
   "    float inside = step(abs(c.x), hx) * step(abs(c.y), vy);\n"
   "    float boost = 1.0 + 3.0 * (1.0 - vy) * (1.0 - vy) + 2.0 * (1.0 - hx);\n"
   /* outside the raster the frame shows at half brightness */
   "    col = mix(graw * 0.5, col * boost, inside);\n"
   "    vec3 glow = vec3(0.6, 1.0, 0.75);\n"
   "    float line = exp(-abs(v_px.y - u_size.y * 0.5) / 2.5) * step(abs(c.x), hx) * (1.0 - vy);\n"
   "    float dot_ = exp(-length(v_px - u_size * 0.5) / (3.0 + 30.0 * hx)) * (1.0 - vy) * (1.0 - hx);\n"
   "    col += glow * (line * 1.2 + dot_ * 1.5) * step(0.001, hx);\n"
   "    col = hud(col, v_px);\n"
   "    frag = vec4(col * u_fade, 1.0);\n"
   "    return;\n"
   "  }\n"
   "  vec2 pivot_screen = u_pivot - u_cam;\n"
   "  vec2 w = u_pivot + (v_px - pivot_screen) / u_zoom;\n"
   "  vec2 rel = w - u_pivot;\n"
   "  float dist = length(rel);\n"
   "  vec3 col = vec3(0.035, 0.040, 0.055);\n"
   "  col += vec3(0.02) * (1.0 - clamp(dist / S, 0.0, 1.0));\n"
   "\n"
   /* the industrial monitor, behind everything: a riveted bezel around a
    * colour CRT */
   "  {\n"
   "    vec2 hb = u_mon_rect.zw * 0.5;\n"
   "    vec2 mc = u_mon_rect.xy + hb;\n"
   "    vec2 mp = w - mc;\n"
   "    float inset = u_mon_rect.z * 0.075000;\n"
   "    float bd = rrect(mp, hb, inset * 0.6);\n"
   "    if (bd < 1.0) {\n"
   "      float sd = rrect(mp, hb - inset, inset * 0.45);\n"
   "      float bez = 1.0 - smoothstep(-1.0, 1.0, bd);\n"
   "      float scr = 1.0 - smoothstep(-1.0, 1.0, sd);\n"
   /* brushed metal: a vertical gradient, a bright outer bevel, and a dark
    * lip where it steps down to the glass */
   "      vec3 bcol = vec3(0.17, 0.18, 0.19) * (1.15 - 0.35 * (mp.y / hb.y + 1.0) * 0.5);\n"
   "      bcol += vec3(0.10) * (1.0 - smoothstep(0.0, inset * 0.25, -bd));\n"
   "      bcol *= 1.0 - 0.55 * (1.0 - smoothstep(0.0, inset * 0.4, sd));\n"
   /* four hex-headed rivets in the corners */
   "      vec2 rp = abs(mp) - (hb - inset * 0.5);\n"
   "      float rr = inset * 0.20;\n"
   "      float rd = length(rp) - rr;\n"
   "      float riv = 1.0 - smoothstep(-1.0, 1.0, rd);\n"
   "      vec3 rcol = vec3(0.30, 0.31, 0.33) * (1.0 - 0.6 * smoothstep(0.0, rr, length(rp + vec2(rr * 0.4))));\n"
   "      rcol = mix(rcol, vec3(0.12), smoothstep(rr * 0.75, rr * 0.9, length(rp)));\n"
   "      bcol = mix(bcol, rcol, riv);\n"
   "      col = mix(col, bcol, bez);\n"
   /* the tube: barrel-distorted picture, dark green phosphor, scanlines,
    * a vignette, and the power-on line opening out */
   "      vec2 hs = hb - inset;\n"
   "      vec2 su = mp / hs;\n"
   "      su *= 1.0 + 0.07 * dot(su, su);\n"
   "      float open = u_mon;\n"
   "      float inside = step(abs(su.x), 1.0) * step(abs(su.y), open);\n"
   "      vec3 pcol = vec3(0.020, 0.095, 0.050);\n"
   "      pcol *= 1.0 - 0.45 * dot(su, su) * 0.5;\n"
   "      pcol *= 0.80 + 0.20 * sin(su.y * hs.y * 1.3);\n"
   "      pcol *= 1.0 + 4.0 * (1.0 - open) * (1.0 - open);\n"
   "      float line = exp(-abs(su.y) * hs.y / 2.5) * (1.0 - open) * step(0.001, open);\n"
   "      pcol += vec3(0.6, 1.0, 0.75) * line * 1.5;\n"
   "      vec3 gcol = vec3(0.010, 0.018, 0.014) + pcol * inside;\n"
   /* soft reflection on the glass, up and to the left */
   "      gcol += vec3(0.07, 0.08, 0.09) * (1.0 - smoothstep(0.0, 1.3, length(su - vec2(-0.7, -0.8))));\n"
   "      col = mix(col, gcol, scr);\n"
   "    }\n"
   "  }\n"
   "\n"
   /* spheres */
   "  for (int i = 0; i < u_count; i++) {\n"
   "    float a = u_spheres[i].x;\n"
   "    float sc = u_spheres[i].y;\n"
   "    float stretch = u_spheres[i].z;\n"
   "    float dim = u_spheres[i].w;\n"
   "    vec2 c = u_pivot + u_radius * vec2(-cos(a), -sin(a));\n"
   "    vec2 q = w - c;\n"
   /* basis along the tangent (clockwise) and the normal */
   "    vec2 n = normalize(c - u_pivot);\n"
   "    vec2 t = vec2(-n.y, n.x);\n"
   "    vec2 l = vec2(dot(q, t) / (1.0 + stretch), dot(q, n));\n"
   "    float R = u_sphere_r * sc;\n"
   "    float r = length(l) / R;\n"
   "    if (r > 1.05) continue;\n"
   "    float edge = 1.0 - smoothstep(0.96, 1.04, r);\n"
   /* liquid body: lit from the top-left of the screen */
   "    vec2 light = normalize(vec2(-0.4, -0.7));\n"
   "    float z = sqrt(max(0.0, 1.0 - r * r));\n"
   "    vec3 nrm = normalize(vec3(q / R, z));\n"
   "    float diff = 0.55 + 0.45 * max(0.0, dot(nrm.xy, light) * 0.6 + nrm.z * 0.8);\n"
   "    vec3 base = u_colors[i] * diff;\n"
   "    base = mix(base, base * 0.55, smoothstep(0.6, 1.0, r));\n"
   /* box art, submerged: a round vignette into the liquid's rim, lit
    * with the body and magnified a touch toward the centre */
   "    if (u_art[i] >= 0.0) {\n"
   "      vec2 uv = 0.5 + (q / R) * 0.5 * (0.90 + 0.06 * r * r);\n"
   "      vec4 art = texture(u_art_tex, vec3(uv, u_art[i]));\n"
   "      float am = (1.0 - smoothstep(0.72, 1.0, r)) * art.a;\n"
   "      base = mix(base, art.rgb * (0.70 + 0.40 * diff), am);\n"
   "    }\n"
   /* specular highlight near the top */
   "    vec2 hl = (q / R) - vec2(-0.30, -0.42);\n"
   "    float sp = exp(-dot(hl, hl) * 14.0);\n"
   "    base += vec3(1.0) * sp * 0.9;\n"
   "    base *= (1.0 - dim * 0.45);\n"
   "    float alpha = edge * (0.82 + 0.18 * z) * (1.0 - min(stretch, 1.5) * 0.4) * u_alpha[i];\n"
   "    col = mix(col, base, alpha);\n"
   "  }\n"
   /* glass tube, over the spheres so it reads as holding them: a translucent
    * band with an inner specular stripe */
   "  float tube_w = u_sphere_r * 1.35;\n"
   "  float d = abs(dist - u_radius);\n"
   "  float body = 1.0 - smoothstep(tube_w - 1.5, tube_w + 1.5, d);\n"
   "  float rim  = smoothstep(tube_w - 3.0, tube_w, d) * body;\n"
   "  float spec = exp(-pow((dist - (u_radius - tube_w * 0.55)) / (tube_w * 0.10), 2.0));\n"
   "  vec3 glass = vec3(0.55, 0.70, 0.85);\n"
   "  col = mix(col, glass, body * 0.06);\n"
   "  col += glass * rim * 0.35;\n"
   "  col += vec3(1.0) * spec * 0.25 * body;\n"
   "  col = hud(col, v_px);\n"
   "\n"
   "  frag = vec4(col * u_fade, 1.0);\n"
   "}\n";

static GLuint ramen_compile(GLenum type, const char *src)
{
   GLint ok     = 0;
   GLuint sh    = glCreateShader(type);
   glShaderSource(sh, 1, &src, NULL);
   glCompileShader(sh);
   glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
   if (!ok)
   {
      char log[1024];
      glGetShaderInfoLog(sh, sizeof(log), NULL, log);
      RARCH_ERR("[ramen] shader compile failed: %s\n", log);
      glDeleteShader(sh);
      return 0;
   }
   return sh;
}

static bool ramen_gl_init(ramen_handle_t *rm)
{
   GLint ok   = 0;
   GLuint vs  = ramen_compile(GL_VERTEX_SHADER, ramen_vert);
   GLuint fs  = ramen_compile(GL_FRAGMENT_SHADER, ramen_frag);

   if (!vs || !fs)
      return false;

   rm->prog = glCreateProgram();
   glAttachShader(rm->prog, vs);
   glAttachShader(rm->prog, fs);
   glLinkProgram(rm->prog);
   glDeleteShader(vs);
   glDeleteShader(fs);
   glGetProgramiv(rm->prog, GL_LINK_STATUS, &ok);
   if (!ok)
   {
      char log[1024];
      glGetProgramInfoLog(rm->prog, sizeof(log), NULL, log);
      RARCH_ERR("[ramen] program link failed: %s\n", log);
      glDeleteProgram(rm->prog);
      rm->prog = 0;
      return false;
   }

   rm->u_size     = glGetUniformLocation(rm->prog, "u_size");
   rm->u_pivot    = glGetUniformLocation(rm->prog, "u_pivot");
   rm->u_cam      = glGetUniformLocation(rm->prog, "u_cam");
   rm->u_zoom     = glGetUniformLocation(rm->prog, "u_zoom");
   rm->u_fade     = glGetUniformLocation(rm->prog, "u_fade");
   rm->u_radius   = glGetUniformLocation(rm->prog, "u_radius");
   rm->u_sphere_r = glGetUniformLocation(rm->prog, "u_sphere_r");
   rm->u_count    = glGetUniformLocation(rm->prog, "u_count");
   rm->u_spheres  = glGetUniformLocation(rm->prog, "u_spheres");
   rm->u_colors   = glGetUniformLocation(rm->prog, "u_colors");
   rm->u_alpha    = glGetUniformLocation(rm->prog, "u_alpha");
   rm->u_mon      = glGetUniformLocation(rm->prog, "u_mon");
   rm->u_mon_rect = glGetUniformLocation(rm->prog, "u_mon_rect");
   rm->u_art_tex  = glGetUniformLocation(rm->prog, "u_art_tex");
   rm->u_art      = glGetUniformLocation(rm->prog, "u_art");
   rm->u_game     = glGetUniformLocation(rm->prog, "u_game");
   rm->u_game_rect = glGetUniformLocation(rm->prog, "u_game_rect");
   rm->u_game_uv  = glGetUniformLocation(rm->prog, "u_game_uv");
   rm->u_game_flip = glGetUniformLocation(rm->prog, "u_game_flip");
   rm->u_ig       = glGetUniformLocation(rm->prog, "u_ig");
   rm->u_ig_open  = glGetUniformLocation(rm->prog, "u_ig_open");
   rm->u_degauss  = glGetUniformLocation(rm->prog, "u_degauss");
   rm->u_vial     = glGetUniformLocation(rm->prog, "u_vial");
   rm->u_vial_rgb = glGetUniformLocation(rm->prog, "u_vial_rgb");
   rm->u_vial_dim = glGetUniformLocation(rm->prog, "u_vial_dim");
   rm->u_hud      = glGetUniformLocation(rm->prog, "u_hud");
   rm->u_phosphor = glGetUniformLocation(rm->prog, "u_phosphor");
   rm->u_time     = glGetUniformLocation(rm->prog, "u_time");

   /* the box art array, empty until the game view asks for it */
   glGenTextures(1, &rm->art_tex);
   glBindTexture(GL_TEXTURE_2D_ARRAY, rm->art_tex);
   glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, RAMEN_ART_SIZE,
         RAMEN_ART_SIZE, RAMEN_ART_LAYERS);
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
   memset(rm->art, 0, sizeof(rm->art));
   for (ok = 0; ok < RAMEN_ART_LAYERS; ok++)
      rm->art[ok].sys = -1;

   /* The vertex shader derives the fullscreen triangle from gl_VertexID,
    * but a bound VAO is still required to draw in ES 3. */
   glGenVertexArrays(1, &rm->vao);
   rm->gl_ready = true;
   return true;
}

static void ramen_gl_free(ramen_handle_t *rm)
{
   if (rm->prog)
      glDeleteProgram(rm->prog);
   if (rm->vao)
      glDeleteVertexArrays(1, &rm->vao);
   if (rm->art_tex)
      glDeleteTextures(1, &rm->art_tex);
   rm->prog     = 0;
   rm->vao      = 0;
   rm->art_tex  = 0;
   rm->gl_ready = false;
}

/* ---- helpers --------------------------------------------------------- */

static float ramen_smooth(float t)
{
   t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
   return t * t * (3.0f - 2.0f * t);
}

/* Ease-out: all the speed at the start, settling into place. */
static float ramen_burst(float t)
{
   t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
   t = 1.0f - t;
   return 1.0f - t * t * t;
}

/* One step of an underdamped spring, semi-implicit Euler. */
static void ramen_spring(float *x, float *v, float target, float hz,
      float zeta, float dt)
{
   float w   = 2.0f * (float)M_PI * hz;
   float acc = w * w * (target - *x) - 2.0f * zeta * w * *v;
   *v += acc * dt;
   *x += *v * dt;
}

/* ---- sounds ------------------------------------------------------------ */

static float ramen_noise(void)
{
   static uint32_t x = 0x9e3779b9u;
   x ^= x << 13; x ^= x >> 17; x ^= x << 5;
   return (float)(x & 0xffff) / 32768.0f - 1.0f;
}

static bool ramen_snd_add(unsigned slot, const char *name,
      const int16_t *pcm, unsigned frames)
{
   audio_mixer_stream_params_t params;
   size_t data = (size_t)frames * 2;
   size_t n    = 44 + data;
   uint8_t *b  = (uint8_t*)malloc(n);
   char basename[16];
   bool ok;

   if (!b)
      return false;
   /* RIFF / WAVE, 16-bit mono */
#define W32(o, v) do { uint32_t _v = (v); b[o] = _v & 0xff; b[o+1] = (_v >> 8) & 0xff; \
      b[o+2] = (_v >> 16) & 0xff; b[o+3] = (_v >> 24) & 0xff; } while (0)
#define W16(o, v) do { uint16_t _v = (v); b[o] = _v & 0xff; b[o+1] = (_v >> 8) & 0xff; } while (0)
   memcpy(b, "RIFF", 4); W32(4, (uint32_t)(n - 8)); memcpy(b + 8, "WAVE", 4);
   memcpy(b + 12, "fmt ", 4); W32(16, 16); W16(20, 1); W16(22, 1);
   W32(24, RAMEN_SND_RATE); W32(28, RAMEN_SND_RATE * 2); W16(32, 2); W16(34, 16);
   memcpy(b + 36, "data", 4); W32(40, (uint32_t)data);
#undef W32
#undef W16
   memcpy(b + 44, pcm, data);
   strlcpy(basename, name, sizeof(basename));
   memset(&params, 0, sizeof(params));
   params.buf                 = b;
   params.bufsize             = n;
   params.basename            = basename;
   params.volume              = 1.0f;
   params.slot_selection_type = AUDIO_MIXER_SLOT_SELECTION_MANUAL;
   params.slot_selection_idx  = slot;
   params.stream_type         = AUDIO_STREAM_TYPE_SYSTEM;
   params.type                = AUDIO_MIXER_TYPE_WAV;
   params.state               = AUDIO_STREAM_STATE_STOPPED;
   ok = audio_driver_mixer_add_stream(&params);   /* copies the buffer */
   free(b);
   return ok;
}

/* Synthesises one cue: frames at RAMEN_SND_RATE. */
static unsigned ramen_snd_synth(enum ramen_sound which, int16_t *pcm,
      unsigned max)
{
   const float sr = (float)RAMEN_SND_RATE;
   double phase = 0.0;
   unsigned n, i;
   float len;

   if (which == SND_TICK1 || which == SND_TICK2)
      which = SND_TICK;
   switch (which)
   {
      case SND_TICK:    len = 0.035f; break;
      case SND_CONFIRM: len = 0.14f;  break;
      case SND_BACK:    len = 0.12f;  break;
      case SND_THUNK:   len = 0.75f;  break;
      case SND_POWER:   len = 0.30f;  break;
      default:          len = 0.22f;  break;
   }
   n = MIN(max, (unsigned)(len * sr));
   for (i = 0; i < n; i++)
   {
      float t = (float)i / sr;
      float v = 0.0f, f = 0.0f;

      switch (which)
      {
         case SND_TICK:
            /* a prize-wheel peg: a short bright click */
            f = 1900.0f;
            v = sinf((float)phase) * expf(-t * 140.0f) * 0.7f
              + ramen_noise() * expf(-t * 400.0f) * 0.35f;
            break;
         case SND_CONFIRM:
            /* a rising two-step blip */
            f = t < 0.06f ? 620.0f : 930.0f;
            v = sinf((float)phase) * (1.0f - expf(-t * 900.0f)) * expf(-t * 16.0f) * 0.6f;
            break;
         case SND_BACK:
            /* and the same, falling */
            f = t < 0.05f ? 820.0f : 520.0f;
            v = sinf((float)phase) * (1.0f - expf(-t * 900.0f)) * expf(-t * 20.0f) * 0.5f;
            break;
         case SND_THUNK:
            /* the degauss: a thud, then a mains hum dying away */
            f = 60.0f;
            v = sinf(2.0f * (float)M_PI * 68.0f * t) * expf(-t * 24.0f) * 0.9f
              + ramen_noise() * expf(-t * 110.0f) * 0.45f;
            if (t > 0.03f)
               v += (sinf((float)phase) + 0.45f * sinf(2.0f * (float)phase)
                     + 0.2f * sinf(3.0f * (float)phase))
                  * 0.28f * expf(-(t - 0.03f) * 4.5f);
            break;
         case SND_POWER:
            /* the CRT coming up: a swell and a high ping */
            f = 160.0f + 900.0f * MIN(1.0f, t / 0.18f);
            v = sinf((float)phase) * (1.0f - expf(-t * 50.0f)) * expf(-t * 11.0f) * 0.45f;
            if (t > 0.05f)
               v += sinf(2.0f * (float)M_PI * 2600.0f * t) * expf(-(t - 0.05f) * 35.0f) * 0.18f;
            break;
         default:
            /* power off: a falling sweep */
            f = 950.0f - 820.0f * MIN(1.0f, t / 0.16f);
            v = sinf((float)phase) * (1.0f - expf(-t * 900.0f)) * expf(-t * 15.0f) * 0.5f;
            break;
      }
      phase += 2.0 * M_PI * f / sr;
      v *= RAMEN_SND_VOL;
      v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
      pcm[i] = (int16_t)(v * 32767.0f);
   }
   /* No cue ends at exactly zero: fade the last few ms out, and the
    * first ms in, so neither edge pops. */
   {
      unsigned tail = MIN(n, (unsigned)(0.008f * sr));
      unsigned head = MIN(n, (unsigned)(0.001f * sr));
      for (i = 0; i < tail; i++)
         pcm[n - 1 - i] = (int16_t)(pcm[n - 1 - i] * (float)i / (float)tail);
      for (i = 0; i < head; i++)
         pcm[i] = (int16_t)(pcm[i] * (float)i / (float)head);
   }
   return n;
}

static void ramen_snd_load(void)
{
   static const char *names[] = { "tick", "tick1", "tick2", "confirm", "back",
      "thunk", "power", "off" };
   int16_t *pcm = (int16_t*)malloc(RAMEN_SND_RATE * sizeof(int16_t));
   unsigned k;

   if (!pcm)
      return;
   for (k = SND_TICK; k < SND_END; k++)
   {
      unsigned n = ramen_snd_synth((enum ramen_sound)k, pcm, RAMEN_SND_RATE);
      if (!ramen_snd_add(k, names[k - SND_TICK], pcm, n))
         RARCH_WARN("[ramen] could not load the %s sound\n", names[k - SND_TICK]);
   }
   free(pcm);
}

static void ramen_snd_play(enum ramen_sound which)
{
   static unsigned tick_slot = 0;

   if (which == SND_TICK)
   {
      which     = (enum ramen_sound)(SND_TICK + tick_slot);
      tick_slot = (tick_slot + 1) % 3;
   }
   if (audio_driver_mixer_get_stream_state(which) == AUDIO_STREAM_STATE_NONE)
      ramen_snd_load();
   /* The menu-sound path keeps the stream once the cue has finished.
    * The plain one tears it down, and reloading every cue on the next
    * play of that slot stalled a frame every third tick. */
   audio_driver_mixer_play_menu_sound(which);
}

/* ---- battery and volume ------------------------------------------------ */

/* The first line of a small file, or false if it can't be read. */
static bool ramen_read_line(const char *path, char *buf, size_t len)
{
   FILE *f = fopen(path, "r");
   bool ok = false;

   if (!f)
      return false;
   if (fgets(buf, (int)len, f))
   {
      buf[strcspn(buf, "\n")] = '\0';
      ok = true;
   }
   fclose(f);
   return ok;
}

static long ramen_read_long(const char *path)
{
   char buf[32];

   return ramen_read_line(path, buf, sizeof(buf)) ? atol(buf) : -1;
}

/*
 * The gauge re-derives its percentage from the open-circuit voltage, so a
 * battery the charger has just called Full reads ~95%, and unplugging it
 * dropped the vial straight there. Instead, while the charger says Full,
 * show 100% and note the coulomb counter; on battery, count down from 100 by
 * what has really been drained (charge_now and the gauge's own capacity,
 * both uAh) until that meets the gauge, which then takes over. On battery
 * the number never goes up: the OCV recovers a little at rest. Kept outside
 * the handle, which RetroArch can rebuild.
 */
static struct
{
   bool counting;
   bool topping_up;	/* Charging straight after Full, still on the cable */
   long full_uah;
   int  shown;
} ramen_bat = { false, false, 0, -1 };

static int ramen_battery_pct(int gauge, const char *status)
{
   int pct = gauge;

   if (string_is_equal(status, "Full"))
   {
      ramen_bat.counting   = true;
      ramen_bat.topping_up = true;
      ramen_bat.full_uah   = ramen_read_long(RAMEN_FGU_PATH "/charge_now");
      ramen_bat.shown      = 100;
      return 100;
   }
   /* a charger that called it Full tops up now and then: still full */
   if (string_is_equal(status, "Charging") && ramen_bat.topping_up)
      return 100;
   ramen_bat.topping_up = false;
   if (string_is_equal(status, "Charging") || gauge < 0)
   {
      ramen_bat.counting = false;
      ramen_bat.shown    = gauge;
      return gauge;
   }

   if (ramen_bat.counting)
   {
      long now = ramen_read_long(RAMEN_FGU_PATH "/charge_now");
      long cap = ramen_read_long(RAMEN_FGU_PATH "/energy_full_design");

      if (now >= 0 && cap > 0)
      {
         long left = 100 - (ramen_bat.full_uah - now) * 100 / cap;

         if (left > gauge)
            pct = (int)MIN(100, left);
         else
            ramen_bat.counting = false;
      }
      else
         ramen_bat.counting = false;
   }
   if (ramen_bat.shown >= 0 && pct > ramen_bat.shown)
      pct = ramen_bat.shown;
   ramen_bat.shown = pct;
   return pct;
}

/* While data is lent to the host (or unreadable) the volume keeps its last
 * value. */
static void ramen_read_status(ramen_handle_t *rm)
{
   char buf[32];
   int gauge = -1;

   if (ramen_read_line(RAMEN_FGU_PATH "/capacity", buf, sizeof(buf)))
      gauge = MAX(0, MIN(100, atoi(buf)));
   if (!ramen_read_line(RAMEN_CHARGER_PATH "/status", buf, sizeof(buf)))
      buf[0] = '\0';
   rm->bat_charging = string_is_equal(buf, "Charging");
   rm->bat_pct      = ramen_battery_pct(gauge, buf);
   if (ramen_read_line(RAMEN_VOLUME_PATH, buf, sizeof(buf)))
      rm->vol = MAX(0, MIN(RAMEN_VOLUME_MAX, atoi(buf)));
}

/* ---- USB storage ------------------------------------------------------- */

static bool ramen_mounted(const char *dir)
{
   FILE *f = fopen("/proc/mounts", "r");
   char line[512];
   char want[PATH_MAX_LENGTH];
   bool found = false;

   if (!f)
      return false;
   snprintf(want, sizeof(want), " %s ", dir);
   while (fgets(line, sizeof(line), f))
      if (strstr(line, want))
      {
         found = true;
         break;
      }
   fclose(f);
   return found;
}

/* Whether a card is in the slot: an SD-type MMC disk. */
static bool ramen_sd_present(void)
{
   DIR *d = opendir("/sys/class/block");
   struct dirent *e;
   bool found = false;

   if (!d)
      return false;
   while (!found && (e = readdir(d)))
   {
      char path[PATH_MAX_LENGTH];
      char type[8];

      if (strncmp(e->d_name, "mmcblk", 6) || strchr(e->d_name, 'p'))
         continue;
      snprintf(path, sizeof(path), "/sys/class/block/%s/device/type",
            e->d_name);
      found = ramen_read_line(path, type, sizeof(type))
         && string_is_equal(type, "SD");
   }
   closedir(d);
   return found;
}

static void ramen_notice(ramen_handle_t *rm, const char *msg, float secs)
{
   strlcpy(rm->notice, msg, sizeof(rm->notice));
   rm->notice_t = secs;
}

/* ---- systems --------------------------------------------------------- */

static int ramen_selected_system(ramen_handle_t *rm);
static void ramen_saver(ramen_handle_t *rm, bool on);

/* Adds a ROM directory to a system unless it has it already. */
static void ramen_sys_add_dir(ramen_system_t *s, const char *dir)
{
   int i;

   if (string_is_empty(dir) || s->ndirs >= RAMEN_SYS_DIRS)
      return;
   for (i = 0; i < s->ndirs; i++)
      if (string_is_equal(s->dirs[i], dir))
         return;
   strlcpy(s->dirs[s->ndirs++], dir, sizeof(s->dirs[0]));
}

/* A user core's colour: a hue hashed from its name, so it keeps it from
 * boot to boot, at about the built-ins' saturation and brightness. */
static void ramen_sys_color(const char *name, float *rgb)
{
   const float hi = 0.95f, lo = 0.22f;
   uint32_t h     = 2166136261u;
   float hue, up, dn;
   int i;

   for (; *name; name++)
      h = (h ^ (uint8_t)*name) * 16777619u;
   hue = (float)(h % 360u) / 60.0f;
   i   = (int)hue;
   up  = lo + (hi - lo) * (hue - i);
   dn  = hi - (hi - lo) * (hue - i);
   switch (i)
   {
      case 0:  rgb[0] = hi; rgb[1] = up; rgb[2] = lo; break;
      case 1:  rgb[0] = dn; rgb[1] = hi; rgb[2] = lo; break;
      case 2:  rgb[0] = lo; rgb[1] = hi; rgb[2] = up; break;
      case 3:  rgb[0] = lo; rgb[1] = dn; rgb[2] = hi; break;
      case 4:  rgb[0] = up; rgb[1] = lo; rgb[2] = hi; break;
      default: rgb[0] = hi; rgb[1] = lo; rgb[2] = dn; break;
   }
}

/* What a core without an .info says about itself, from
 * retro_get_system_info(), which libretro allows before retro_init(). Only
 * fills what is still empty. */
static void ramen_probe_core(const char *path, char *name, size_t name_len,
      char *exts, size_t exts_len)
{
   struct retro_system_info info;
   void (*get_info)(struct retro_system_info*);
   dylib_t lib = dylib_load(path);

   if (!lib)
   {
      RARCH_ERR("[ramen] %s: %s\n", path, dylib_error());
      return;
   }
   get_info = (void (*)(struct retro_system_info*))
      dylib_proc(lib, "retro_get_system_info");
   if (get_info)
   {
      memset(&info, 0, sizeof(info));
      get_info(&info);
      if (!*name && info.library_name)
         strlcpy(name, info.library_name, name_len);
      if (!*exts && info.valid_extensions)
         strlcpy(exts, info.valid_extensions, exts_len);
   }
   dylib_close(lib);
}

/* The user cores in one root's cores directory, <name>_libretro.so. One
 * reads roms/<name>, and with a paired <name>_libretro.info also
 * roms/<systemid>, under every root; neither is created, since they are
 * the user's to make. One that shares a built-in's core file name, or whose
 * name or systemid is a built-in's directory, replaces that built-in's core
 * and keeps its slot, label, colour and directory. A core file name already
 * seen in an earlier root is skipped. */
static void ramen_scan_cores(ramen_handle_t *rm, const char *root)
{
   static const char suffix[] = "_libretro.so";
   struct string_list *cores;
   char dir[PATH_MAX_LENGTH];
   size_t i;
   int b;

   fill_pathname_join_special(dir, root, "cores", sizeof(dir));
   if (!(cores = dir_list_new(dir, "so", false, false, false, false)))
      return;
   dir_list_sort(cores, false);
   for (i = 0; i < cores->size; i++)
   {
      const char *path  = cores->elems[i].data;
      const char *file  = path_basename(path);
      size_t n          = strlen(file);
      ramen_system_t *s = NULL;
      config_file_t *conf;
      char shortname[64];
      char sysid[64]    = "";
      char name[64]     = "";
      char exts[512]    = "";
      char info[PATH_MAX_LENGTH];

      if (n <= STRLEN_CONST(suffix)
            || !string_is_equal(file + n - STRLEN_CONST(suffix), suffix))
         continue;
      for (b = 0; b < rm->nsys; b++)
         if (     rm->sys[b].custom
               && string_is_equal(path_basename(rm->sys[b].core), file))
            break;
      if (b < rm->nsys)
      {
         RARCH_LOG("[ramen] %s: already loaded from %s, skipped\n", path,
               rm->sys[b].core);
         continue;
      }
      strlcpy(shortname, file,
            MIN(sizeof(shortname), n - STRLEN_CONST(suffix) + 1));

      strlcpy(info, path, sizeof(info));
      path_remove_extension(info);
      strlcat(info, ".info", sizeof(info));
      if (     path_is_valid(info)
            && (conf = config_file_new_from_path_to_string(info)))
      {
         config_get_array(conf, "systemid", sysid, sizeof(sysid));
         config_get_array(conf, "corename", name, sizeof(name));
         config_get_array(conf, "supported_extensions", exts, sizeof(exts));
         config_file_free(conf);
      }
      if (!*name || !*exts)
         ramen_probe_core(path, name, sizeof(name), exts, sizeof(exts));
      if (!*exts)
      {
         RARCH_WARN("[ramen] %s: no content extensions, skipped\n", file);
         continue;
      }

      for (b = 0; b < RAMEN_BUILTINS && !s; b++)
         if (     !rm->sys[b].custom
               && (   string_is_equal(file, ramen_builtins[b].core)
                   || string_is_equal(shortname, ramen_builtins[b].dir)
                   || string_is_equal(sysid, ramen_builtins[b].dir)))
            s = &rm->sys[b];
      if (!s)
      {
         if (rm->nsys >= RAMEN_SYSTEMS_MAX)
            break;
         s = &rm->sys[rm->nsys++];
         memset(s, 0, sizeof(*s));
         strlcpy(s->name, *name ? name : shortname, sizeof(s->name));
         ramen_sys_color(shortname, s->rgb);
      }
      s->custom = true;
      strlcpy(s->core, path, sizeof(s->core));
      strlcpy(s->exts, exts, sizeof(s->exts));
      ramen_sys_add_dir(s, shortname);
      ramen_sys_add_dir(s, sysid);
      RARCH_LOG("[ramen] %s: %s, in roms/%s%s%s\n", s->name, path,
            s->dirs[0], s->ndirs > 1 ? " and " : "",
            s->ndirs > 1 ? s->dirs[1] : "");
   }
   string_list_free(cores);
}

/* The built-ins, then the user cores of every root. */
static void ramen_scan_systems(ramen_handle_t *rm)
{
   int b;

   rm->nsys = RAMEN_BUILTINS;
   for (b = 0; b < RAMEN_BUILTINS; b++)
   {
      ramen_system_t *s = &rm->sys[b];

      memset(s, 0, sizeof(*s));
      strlcpy(s->name, ramen_builtins[b].name, sizeof(s->name));
      ramen_sys_add_dir(s, ramen_builtins[b].dir);
      fill_pathname_join_special(s->core, RAMEN_CORE_DIR,
            ramen_builtins[b].core, sizeof(s->core));
      strlcpy(s->exts, ramen_builtins[b].exts, sizeof(s->exts));
      memcpy(s->rgb, ramen_builtins[b].rgb, sizeof(s->rgb));
   }

   for (b = 0; b < RAMEN_ROOTS; b++)
      ramen_scan_cores(rm, ramen_roots[b]);
}

/* Fills RAMEN_BIOS_LINKS with a link to each entry of every root's bios
 * directory, the first root's winning a name both have. Entries are linked
 * whole, so a core writing under a subdirectory writes to the real one; a
 * file a core makes at the top level stays in /run until reboot. */
static void ramen_link_bios(void)
{
   char src[PATH_MAX_LENGTH];
   char link[PATH_MAX_LENGTH];
   struct dirent *e;
   struct stat st;
   DIR *d;
   int r;

   path_mkdir(RAMEN_BIOS_LINKS);
   /* the old links go; whatever a core made here stays */
   if ((d = opendir(RAMEN_BIOS_LINKS)))
   {
      while ((e = readdir(d)))
      {
         fill_pathname_join_special(link, RAMEN_BIOS_LINKS, e->d_name,
               sizeof(link));
         if (lstat(link, &st) == 0 && S_ISLNK(st.st_mode))
            unlink(link);
      }
      closedir(d);
   }
   for (r = 0; r < RAMEN_ROOTS; r++)
   {
      char dir[PATH_MAX_LENGTH];

      fill_pathname_join_special(dir, ramen_roots[r], "bios", sizeof(dir));
      if (!(d = opendir(dir)))
         continue;
      while ((e = readdir(d)))
      {
         if (e->d_name[0] == '.')
            continue;
         fill_pathname_join_special(src, dir, e->d_name, sizeof(src));
         fill_pathname_join_special(link, RAMEN_BIOS_LINKS, e->d_name,
               sizeof(link));
         if (lstat(link, &st) != 0 && symlink(src, link) != 0)
            RARCH_WARN("[ramen] cannot link %s\n", src);
      }
      closedir(d);
   }
}

/* The system whose key (dirs[0]) this is, or -1. */
static int ramen_find_system(ramen_handle_t *rm, const char *key)
{
   int i;

   for (i = 0; i < rm->nsys; i++)
      if (string_is_equal(rm->sys[i].dirs[0], key))
         return i;
   return -1;
}

/* /data may have changed under us: forget what was read from it. */
static void ramen_forget_data(ramen_handle_t *rm)
{
   char key[64];
   int i, cur, sel;

   strlcpy(key, rm->sys[ramen_selected_system(rm)].dirs[0], sizeof(key));
   for (i = 0; i < RAMEN_SYSTEMS_MAX; i++)
   {
      if (rm->games[i])
         string_list_free(rm->games[i]);
      rm->games[i]        = NULL;
      rm->games_loaded[i] = false;
   }
   rm->meta_sys = rm->meta_game = -1;
   for (i = 0; i < RAMEN_ART_LAYERS; i++)
   {
      rm->art[i].sys     = -1;
      rm->art[i].pending = false;
   }
   /* the decoder's queue goes too; what it is on now is dropped on return */
   if (rm->art_thread)
   {
      slock_lock(rm->art_lock);
      rm->art_gen++;
      for (i = 0; i < RAMEN_ART_JOBS; i++)
         if (rm->art_jobs[i].state == ART_QUEUED)
            rm->art_jobs[i].state = ART_FREE;
      slock_unlock(rm->art_lock);
   }
   rm->game = 0;

   /* The user's cores and BIOS may have come or gone too. The wheel stays
    * on the system it was on, shifted by whole slots so nothing visibly
    * turns. */
   ramen_link_bios();
   ramen_scan_systems(rm);
   cur = ramen_selected_system(rm);
   if ((sel = ramen_find_system(rm, key)) >= 0 && sel != cur)
   {
      rm->wheel_target -= (sel - cur) * RAMEN_SLOT_DEG;
      rm->wheel        -= (sel - cur) * RAMEN_SLOT_DEG;
   }
}

/* Runs the gadget script: from render(), since it blocks for a moment. */
static void ramen_usb_run(ramen_handle_t *rm)
{
   int req   = rm->usb_req;
   bool back = (req == 3);
   int r;

   rm->usb_req = 0;
   if (req == 1)
   {
      if (system("sdcard umount") != 0)
      {
         ramen_notice(rm, "Could not enter Storage Mode: the SD card is busy", 4.0f);
         return;
      }
      r = system("usb-gadget storage on");
      if (r == 0)
      {
         rm->state      = RAMEN_USB;
         rm->usb_poll   = 0.0f;
         rm->sd_present = ramen_sd_present();
         rm->notice[0]  = '\0';
      }
      else
      {
         r = system("sdcard mount");
         ramen_notice(rm, "Could not enter Storage Mode: /data is busy", 4.0f);
      }
   }
   else if (req == 2)
   {
      r = system("usb-gadget storage off");
      if (r == 0 || ramen_mounted("/data"))
         back = true;
      else
         ramen_notice(rm, "The host still holds the drive: eject it there first", 4.0f);
   }
   /* /data is back (here or from the gadget's eject watcher): the card
    * next, then everything read from either is read again. */
   if (back)
   {
      r = system("sdcard mount");
      rm->state = RAMEN_SYSTEM;
      ramen_forget_data(rm);
      ramen_snd_play(SND_BACK);
      if (r != 0)
         ramen_notice(rm, "Could not mount the SD card (exFAT or FAT only)", 4.0f);
   }
}

/* ---- persistence ----------------------------------------------------- */

static int ramen_load_last_system(ramen_handle_t *rm)
{
   FILE *f = fopen(RAMEN_STATE_PATH, "r");
   char key[64];
   int sys = -1;

   if (!f)
      return 0;
   if (fscanf(f, "%63s", key) == 1)
      sys = ramen_find_system(rm, key);
   fclose(f);
   return sys < 0 ? 0 : sys;
}

static void ramen_save_last_system(ramen_handle_t *rm, int sys)
{
   FILE *f = fopen(RAMEN_STATE_PATH, "w");
   if (!f)
      return;
   fprintf(f, "%s\n", rm->sys[sys].dirs[0]);
   fclose(f);
}

/* ---- games ----------------------------------------------------------- */

/* By file name, so the directories of a system interleave. */
static int ramen_game_cmp(const void *a, const void *b)
{
   const struct string_list_elem *x = (const struct string_list_elem*)a;
   const struct string_list_elem *y = (const struct string_list_elem*)b;
   return strcasecmp(path_basename(x->data), path_basename(y->data));
}

static void ramen_load_games(ramen_handle_t *rm, int sys)
{
   const ramen_system_t *s = &rm->sys[sys];
   struct string_list *list;
   char dir[PATH_MAX_LENGTH];
   int i, r;

   if (rm->games_loaded[sys])
      return;
   rm->games_loaded[sys] = true;

   if (!(list = string_list_new()))
      return;
   /* a directory that does not exist just adds nothing */
   for (r = 0; r < RAMEN_ROOTS; r++)
   {
      char roms[PATH_MAX_LENGTH];

      fill_pathname_join_special(roms, ramen_roots[r], "roms", sizeof(roms));
      for (i = 0; i < s->ndirs; i++)
      {
         fill_pathname_join_special(dir, roms, s->dirs[i], sizeof(dir));
         dir_list_append(list, dir, s->exts, false, false, false, false);
      }
   }
   qsort(list->elems, list->size, sizeof(*list->elems), ramen_game_cmp);
   rm->games[sys] = list;
   RARCH_LOG("[ramen] %s: %u games\n", s->name, (unsigned)list->size);
}

static unsigned ramen_game_count(ramen_handle_t *rm, int sys)
{
   return rm->games[sys] ? (unsigned)rm->games[sys]->size : 0;
}

static const char *ramen_game_path(ramen_handle_t *rm, int sys, int idx)
{
   if (!rm->games[sys] || idx < 0 || (unsigned)idx >= rm->games[sys]->size)
      return NULL;
   return rm->games[sys]->elems[idx].data;
}

/* The file name without extension and without the "(USA)" / "[!]" tags. */
static void ramen_game_title(const char *path, char *title, size_t len)
{
   char *p;

   fill_pathname_base(title, path, len);
   path_remove_extension(title);
   for (p = title; *p; p++)
   {
      if (*p == '(' || *p == '[')
      {
         while (p > title && p[-1] == ' ')
            p--;
         *p = '\0';
         break;
      }
   }
}

/* ---- wheel ----------------------------------------------------------- */

/* ---- box art ----------------------------------------------------------- */

/* The cache layer holding a game's art, or -1. Touches it. */
static int ramen_art_find(ramen_handle_t *rm, int sys, int game)
{
   int k;

   for (k = 0; k < RAMEN_ART_LAYERS; k++)
      if (rm->art[k].sys == sys && rm->art[k].game == game)
      {
         rm->art[k].stamp = ++rm->art_stamp;
         return k;
      }
   return -1;
}

/* Centre-crops the image square and resamples it bilinearly into an
 * RGBA buffer of RAMEN_ART_SIZE on a side. Pixels are ARGB words. */
static void ramen_art_resample(const struct texture_image *img,
      uint8_t *out)
{
   unsigned side = MIN(img->width, img->height);
   unsigned ox   = (img->width - side) / 2;
   unsigned oy   = (img->height - side) / 2;
   float step    = (float)side / (float)RAMEN_ART_SIZE;
   unsigned x, y;

   for (y = 0; y < RAMEN_ART_SIZE; y++)
   {
      float fy    = oy + (y + 0.5f) * step - 0.5f;
      int y0      = (int)floorf(fy);
      float wy    = fy - y0;
      int y1      = MIN(y0 + 1, (int)(oy + side - 1));
      y0          = MAX(y0, (int)oy);
      for (x = 0; x < RAMEN_ART_SIZE; x++)
      {
         float fx = ox + (x + 0.5f) * step - 0.5f;
         int x0   = (int)floorf(fx);
         float wx = fx - x0;
         int x1   = MIN(x0 + 1, (int)(ox + side - 1));
         uint32_t p00, p01, p10, p11;
         uint8_t *o = out + (y * RAMEN_ART_SIZE + x) * 4;
         int c;

         x0  = MAX(x0, (int)ox);
         p00 = img->pixels[y0 * img->width + x0];
         p01 = img->pixels[y0 * img->width + x1];
         p10 = img->pixels[y1 * img->width + x0];
         p11 = img->pixels[y1 * img->width + x1];
         /* out is R G B A; the words are A R G B */
         for (c = 0; c < 4; c++)
         {
            int sh  = (c == 3) ? 24 : 16 - 8 * c;
            float v = ((p00 >> sh) & 0xff) * (1 - wx) * (1 - wy)
                    + ((p01 >> sh) & 0xff) * wx * (1 - wy)
                    + ((p10 >> sh) & 0xff) * (1 - wx) * wy
                    + ((p11 >> sh) & 0xff) * wx * wy;
            o[c] = (uint8_t)(v + 0.5f);
         }
      }
   }
}

/* The worker: decodes one job at a time into a RAMEN_ART_SIZE square. */
static void ramen_art_worker(void *data)
{
   ramen_handle_t *rm = (ramen_handle_t*)data;

   slock_lock(rm->art_lock);
   for (;;)
   {
      struct texture_image img;
      uint8_t *buf = NULL;
      int k, job = -1;

      for (k = 0; k < RAMEN_ART_JOBS; k++)
         if (rm->art_jobs[k].state == ART_QUEUED
               && (job < 0 || rm->art_jobs[k].seq < rm->art_jobs[job].seq))
            job = k;
      if (job < 0)
      {
         if (rm->art_quit)
            break;
         scond_wait(rm->art_cond, rm->art_lock);
         continue;
      }
      rm->art_jobs[job].state = ART_BUSY;
      slock_unlock(rm->art_lock);

      memset(&img, 0, sizeof(img));
      if (image_texture_load(&img, rm->art_jobs[job].path) && img.pixels
            && img.width && img.height
            && (buf = (uint8_t*)malloc(RAMEN_ART_SIZE * RAMEN_ART_SIZE * 4)))
         ramen_art_resample(&img, buf);
      image_texture_free(&img);

      slock_lock(rm->art_lock);
      rm->art_jobs[job].buf   = buf;
      rm->art_jobs[job].state = ART_DONE;
   }
   slock_unlock(rm->art_lock);
}

static void ramen_art_start(ramen_handle_t *rm)
{
   rm->art_lock = slock_new();
   rm->art_cond = scond_new();
   if (rm->art_lock && rm->art_cond)
      rm->art_thread = sthread_create(ramen_art_worker, rm);
   if (!rm->art_thread)
      RARCH_WARN("[ramen] no art decoder thread, art is off\n");
}

static void ramen_art_stop(ramen_handle_t *rm)
{
   int k;

   if (rm->art_thread)
   {
      slock_lock(rm->art_lock);
      rm->art_quit = true;
      scond_signal(rm->art_cond);
      slock_unlock(rm->art_lock);
      sthread_join(rm->art_thread);
   }
   for (k = 0; k < RAMEN_ART_JOBS; k++)
      free(rm->art_jobs[k].buf);
   if (rm->art_cond)
      scond_free(rm->art_cond);
   if (rm->art_lock)
      slock_free(rm->art_lock);
   rm->art_thread = NULL;
   rm->art_cond   = NULL;
   rm->art_lock   = NULL;
}

/* The art file for a game, if there is one. */
static bool ramen_art_path(ramen_handle_t *rm, int sys, int game,
      char *path, size_t len)
{
   static const char *exts[] = { "png", "jpg", "jpeg" };
   const char *rom = ramen_game_path(rm, sys, game);
   char dir[PATH_MAX_LENGTH];
   char base[256];
   char name[300];
   size_t e;

   if (!rom)
      return false;
   fill_pathname_basedir(path, rom, len);
   fill_pathname_join_special(dir, path, RAMEN_ART_DIR, sizeof(dir));
   fill_pathname_base(base, rom, sizeof(base));
   path_remove_extension(base);
   for (e = 0; e < ARRAY_SIZE(exts); e++)
   {
      snprintf(name, sizeof(name), "%s.%s", base, exts[e]);
      fill_pathname_join_special(path, dir, name, len);
      if (path_is_valid(path))
         return true;
   }
   return false;
}

/* Claims the least recently used layer for a game and queues its decode.
 * A game with no art file takes a layer too, marked absent, so it is not
 * retried. Returns false if the queue is full, to try again next frame. */
static bool ramen_art_request(ramen_handle_t *rm, int sys, int game)
{
   char path[PATH_MAX_LENGTH];
   int k, lru = 0, job = -1;

   if (!rm->art_thread)
      return true;
   for (k = 1; k < RAMEN_ART_LAYERS; k++)
      if (rm->art[k].stamp < rm->art[lru].stamp)
         lru = k;
   if (!ramen_art_path(rm, sys, game, path, sizeof(path)))
   {
      rm->art[lru].sys     = sys;
      rm->art[lru].game    = game;
      rm->art[lru].present = false;
      rm->art[lru].pending = false;
      rm->art[lru].stamp   = ++rm->art_stamp;
      return true;
   }

   slock_lock(rm->art_lock);
   for (k = 0; k < RAMEN_ART_JOBS; k++)
      if (rm->art_jobs[k].state == ART_FREE)
      {
         job = k;
         break;
      }
   if (job >= 0)
   {
      rm->art_jobs[job].state = ART_QUEUED;
      rm->art_jobs[job].seq   = ++rm->art_seq;
      rm->art_jobs[job].gen   = rm->art_gen;
      rm->art_jobs[job].sys   = sys;
      rm->art_jobs[job].game  = game;
      rm->art_jobs[job].buf   = NULL;
      strlcpy(rm->art_jobs[job].path, path, sizeof(rm->art_jobs[job].path));
      scond_signal(rm->art_cond);
   }
   slock_unlock(rm->art_lock);
   if (job < 0)
      return false;

   rm->art[lru].sys     = sys;
   rm->art[lru].game    = game;
   rm->art[lru].present = false;
   rm->art[lru].pending = true;
   rm->art[lru].stamp   = ++rm->art_stamp;
   return true;
}

/* Uploads what the worker has finished. A result whose layer has since
 * been given to another game, or that predates a rescan, is dropped. */
static void ramen_art_drain(ramen_handle_t *rm)
{
   int k;

   if (!rm->art_thread)
      return;
   slock_lock(rm->art_lock);
   for (k = 0; k < RAMEN_ART_JOBS; k++)
   {
      int l;

      if (rm->art_jobs[k].state != ART_DONE)
         continue;
      if (rm->art_jobs[k].gen == rm->art_gen && rm->gl_ready)
         for (l = 0; l < RAMEN_ART_LAYERS; l++)
         {
            if (!rm->art[l].pending || rm->art[l].sys != rm->art_jobs[k].sys
                  || rm->art[l].game != rm->art_jobs[k].game)
               continue;
            rm->art[l].pending = false;
            if (rm->art_jobs[k].buf)
            {
               glBindTexture(GL_TEXTURE_2D_ARRAY, rm->art_tex);
               glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, l,
                     RAMEN_ART_SIZE, RAMEN_ART_SIZE, 1, GL_RGBA,
                     GL_UNSIGNED_BYTE, rm->art_jobs[k].buf);
               glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
               rm->art[l].present = true;
            }
         }
      free(rm->art_jobs[k].buf);
      rm->art_jobs[k].buf   = NULL;
      rm->art_jobs[k].state = ART_FREE;
   }
   slock_unlock(rm->art_lock);
}

/* Asks for the art of the games around the selection: the ring that is
 * drawn (relative index -last..1) and RAMEN_ART_AHEAD past either end,
 * nearest first, so a sphere has its art by the time it surfaces. */
static void ramen_art_prefetch(ramen_handle_t *rm)
{
   int n    = (int)ramen_game_count(rm, rm->system);
   int last = MIN(n, RAMEN_GAMES_MAX);
   int lo   = -last - RAMEN_ART_AHEAD;
   int hi   = 1 + RAMEN_ART_AHEAD;
   int want = MIN(hi - lo + 1, n);   /* distinct games in the window */
   int d, seen = 0;

   for (d = 0; seen < want; d++)
   {
      int sign;
      for (sign = -1; sign <= 1 && seen < want; sign += 2)
      {
         int i = d * sign;
         int idx;

         if (i < lo || i > hi || (d == 0 && sign > 0))
            continue;
         seen++;
         idx = ((rm->game + i) % n + n) % n;
         if (ramen_art_find(rm, rm->system, idx) >= 0)
            continue;
         if (!ramen_art_request(rm, rm->system, idx))
            return;
      }
   }
}

/* The play time and last-played date of the selected game, out of
 * RetroArch's runtime log, cached until the selection changes. */
static void ramen_refresh_meta(ramen_handle_t *rm)
{
   settings_t *settings = config_get_ptr();
   const char *rom      = ramen_game_path(rm, rm->system, rm->game);
   runtime_log_t *log   = NULL;

   if (rm->meta_sys == rm->system && rm->meta_game == rm->game)
      return;
   rm->meta_sys  = rm->system;
   rm->meta_game = rm->game;
   rm->meta_runtime[0] = rm->meta_last[0] = '\0';
   if (!rom || !settings)
      return;
   log = runtime_log_init(rom, rm->sys[rm->system].core,
         settings->paths.directory_runtime_log,
         settings->paths.directory_playlist,
         !settings->bools.content_runtime_log_aggregate);
   if (log && runtime_log_has_runtime(log))
   {
      if (log->runtime.hours)
         snprintf(rm->meta_runtime, sizeof(rm->meta_runtime),
               "Played %uh %02um", log->runtime.hours, log->runtime.minutes);
      else
         snprintf(rm->meta_runtime, sizeof(rm->meta_runtime),
               "Played %um %02us", log->runtime.minutes,
               log->runtime.seconds);
      if (log->last_played.year)
      {
         /* Relative, in calendar days, since the clock is not something
          * the user sets. Both ends come from the same clock, so a wrong
          * one still gives sensible "days ago" counts across a session;
          * an impossible age means it jumped, and nothing is shown. */
         time_t now = time(NULL);
         struct tm then, today;
         long days;

         memset(&then, 0, sizeof(then));
         then.tm_year  = (int)log->last_played.year - 1900;
         then.tm_mon   = (int)log->last_played.month - 1;
         then.tm_mday  = (int)log->last_played.day;
         then.tm_hour  = 12;
         then.tm_isdst = -1;
         today = *localtime(&now);
         today.tm_hour = 12;
         today.tm_min = today.tm_sec = 0;
         days = (long)((mktime(&today) - mktime(&then)) / 86400.0 + 0.5);
         if (days == 0)
            strlcpy(rm->meta_last, "Last played today", sizeof(rm->meta_last));
         else if (days == 1)
            strlcpy(rm->meta_last, "Last played yesterday", sizeof(rm->meta_last));
         else if (days > 1 && days < 3650)
            snprintf(rm->meta_last, sizeof(rm->meta_last),
                  "Last played %ld days ago", days);
      }
   }
   else
      strlcpy(rm->meta_runtime, "Never played", sizeof(rm->meta_runtime));
   if (log)
      free(log);
}

/* Shortens text to fit a width, in screen pixels at the given scale. */
static void ramen_fit_text(const font_data_t *font, char *text, float scale,
      float max_w)
{
   size_t len = strlen(text);

   if (!font || font_driver_get_message_width((void*)font, text, len, scale)
         <= max_w)
      return;
   while (len > 1)
   {
      len--;
      strcpy(text + len, "...");
      if (font_driver_get_message_width((void*)font, text, len + 3, scale)
            <= max_w)
         return;
   }
}

/* Breaks text into two lines at a word boundary, the first as long as
 * fits; the remainder is shortened to fit. Returns the line count. */
static int ramen_wrap_text(const font_data_t *font, const char *text,
      float scale, float max_w, char *l1, char *l2, size_t len)
{
   const char *best = NULL;
   const char *p;

   strlcpy(l1, text, len);
   l2[0] = '\0';
   if (!font || font_driver_get_message_width((void*)font, text,
            strlen(text), scale) <= max_w)
      return 1;
   for (p = strchr(text, ' '); p; p = strchr(p + 1, ' '))
   {
      if (font_driver_get_message_width((void*)font, text,
               (size_t)(p - text), scale) > max_w)
         break;
      best = p;
   }
   if (!best)
   {
      ramen_fit_text(font, l1, scale, max_w);
      return 1;
   }
   strlcpy(l1, text, MIN(len, (size_t)(best - text) + 1));
   strlcpy(l2, best + 1, len);
   ramen_fit_text(font, l2, scale, max_w);
   return 2;
}

static float ramen_slot_target(int slot)
{
   return RAMEN_SELECT_DEG - slot * RAMEN_SLOT_DEG;
}

/* The slot nearest the selected position for a given wheel angle. Slots
 * are unbounded, like the wheel. */
static int ramen_slot_at(float wheel)
{
   return (int)lroundf((RAMEN_SELECT_DEG - wheel) / RAMEN_SLOT_DEG);
}

static int ramen_slot_system(ramen_handle_t *rm, int slot)
{
   return ((slot % rm->nsys) + rm->nsys) % rm->nsys;
}

static int ramen_selected_system(ramen_handle_t *rm)
{
   return ramen_slot_system(rm, ramen_slot_at(rm->wheel_target));
}

static void ramen_begin_boot_spin(ramen_handle_t *rm, int system)
{
   /* Slot n holds system n, as good as any, since the carousel repeats. */
   rm->wheel_target = ramen_slot_target(system);
   /* Spin clockwise (increasing angle), so start behind the target. The
    * friction integral v0/k must cover the distance minus the spring's
    * one-slot handover. */
   rm->wheel        = rm->wheel_target - RAMEN_BOOT_TURNS * 360.0f;
   rm->wheel_vel    = RAMEN_BOOT_FRICTION *
                      (RAMEN_BOOT_TURNS * 360.0f + RAMEN_BOOT_OVERRUN);
   rm->boot_spin    = true;
   rm->settled      = false;
   rm->label        = 0.0f;
   rm->boot_slot    = (int)floorf((rm->wheel - rm->wheel_target) / RAMEN_SLOT_DEG);
}

static void ramen_snap(ramen_handle_t *rm)
{
   rm->wheel     = rm->wheel_target;
   rm->wheel_vel = 0.0f;
   rm->boot_spin = false;
}

/* One D-pad step. dir > 0 turns the wheel clockwise (increasing angle):
 * the sphere peeking at the bottom edge rides up into the selected slot. */
static void ramen_step(ramen_handle_t *rm, int dir)
{
   if (rm->boot_spin)
      ramen_snap(rm);
   rm->wheel_target += dir * RAMEN_SLOT_DEG;
   rm->settled       = false;
   rm->label         = 0.0f;
   ramen_snd_play(SND_TICK);
}

static float ramen_game_slot(unsigned n)
{
   float slot = n ? 90.0f / (float)n : RAMEN_GAME_SLOT_MAX;
   return MAX(RAMEN_GAME_SLOT_MIN, MIN(RAMEN_GAME_SLOT_MAX, slot));
}

/* One step through the games. dir > 0 moves to the next game, which comes
 * out of the sphere as the queue turns clockwise; the index wraps and the
 * wheel just keeps growing, so the loop is seamless. */
static void ramen_game_step(ramen_handle_t *rm, int dir)
{
   int n = (int)ramen_game_count(rm, rm->system);

   if (n == 0)
      return;
   rm->gcoast = false;
   rm->game = ((rm->game + dir) % n + n) % n;
   rm->gwheel_target += dir * rm->gslot;
   rm->gsettled       = false;
   ramen_snd_play(SND_TICK);
}

static int ramen_entry_action(void *userdata, menu_entry_t *entry,
      size_t i, enum menu_action action);

/* ---- the touch panel -------------------------------------------------- */

static void ramen_touch_open(ramen_handle_t *rm)
{
   int i;

   rm->touch_fd = -1;
   for (i = 0; i < 32; i++)
   {
      char path[32];
      uint8_t evbits[(EV_MAX + 7) / 8];
      uint8_t absbits[(ABS_MAX + 7) / 8];
      struct input_absinfo ax, ay;
      int fd;

      snprintf(path, sizeof(path), "/dev/input/event%d", i);
      fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (fd < 0)
         continue;
      memset(evbits, 0, sizeof(evbits));
      memset(absbits, 0, sizeof(absbits));
      if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0
            || !(evbits[EV_ABS / 8] & (1 << (EV_ABS % 8)))
            || ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0
            || !(absbits[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8)))
            || ioctl(fd, EVIOCGABS(ABS_X), &ax) < 0
            || ioctl(fd, EVIOCGABS(ABS_Y), &ay) < 0)
      {
         close(fd);
         continue;
      }
      rm->touch_fd    = fd;
      rm->touch_max_x = ax.maximum > 0 ? ax.maximum : 1;
      rm->touch_max_y = ay.maximum > 0 ? ay.maximum : 1;
      RARCH_LOG("[ramen] touch: %s, %dx%d\n", path,
            rm->touch_max_x + 1, rm->touch_max_y + 1);
      return;
   }
}

/* The finger in the world: its angle about the pivot in the spheres'
 * convention (degrees), and its distance from it in units of S. */
static bool ramen_touch_world(ramen_handle_t *rm, float *deg, float *dist)
{
   float wx, wy, dx, dy;

   if (!rm->cam_w || rm->cam_zoom <= 0.0f || rm->cam_s <= 0.0f)
      return false;
   wx    = rm->cam_pivot[0] + (rm->touch_x - rm->cam_ps[0]) / rm->cam_zoom;
   wy    = rm->cam_pivot[1] + (rm->touch_y - rm->cam_ps[1]) / rm->cam_zoom;
   dx    = rm->cam_pivot[0] - wx;
   dy    = rm->cam_pivot[1] - wy;
   *dist = sqrtf(dx * dx + dy * dy) / rm->cam_s;
   *deg  = atan2f(dy, dx) * 180.0f / (float)M_PI;
   return *dist > RAMEN_TOUCH_MIN_R;
}

/* The angular half-width of a sphere of the given scale on the tube. */
static float ramen_sphere_deg(float scale)
{
   float r = RAMEN_SPHERE_R * scale / RAMEN_TUBE_R;
   return asinf(MIN(1.0f, r)) * 180.0f / (float)M_PI;
}

static void ramen_touch_down(ramen_handle_t *rm)
{
   float deg, dist;

   rm->drag = 0;
   if (rm->usb_req || rm->launch > 0.0f)
      return;
   if (!ramen_touch_world(rm, &deg, &dist))
      return;
   if (rm->state == RAMEN_SYSTEM)
   {
      if (rm->boot_spin)
         ramen_snap(rm);
      rm->drag    = 1;
      rm->drag_w0 = rm->wheel;
   }
   else if (rm->state == RAMEN_GAME && rm->gintro >= rm->gintro_len)
   {
      rm->drag    = 2;
      rm->drag_w0 = rm->gwheel;
      rm->gcoast  = false;   /* caught */
      rm->gwheel_vel = 0.0f;
   }
   else
      return;
   rm->drag_acc   = 0.0f;
   rm->drag_last  = deg;
   rm->drag_vel   = 0.0f;
   rm->drag_moved = false;
}

/* Turn the games' ring by whole slots, keeping the index in step. */
static void ramen_game_turn(ramen_handle_t *rm, int steps)
{
   int n = (int)ramen_game_count(rm, rm->system);

   if (n == 0 || steps == 0)
      return;
   rm->game           = ((rm->game + steps) % n + n) % n;
   rm->gwheel_target += steps * rm->gslot;
   rm->gsettled       = false;
   ramen_snd_play(SND_TICK);
}

/* The finger is down and moving: the wheel follows it. */
static void ramen_drag_update(ramen_handle_t *rm, float dt)
{
   float deg, dist, w, vel;

   if (!rm->drag || !rm->touch_down)
      return;
   if ((rm->drag == 1 && rm->state != RAMEN_SYSTEM)
         || (rm->drag == 2 && rm->state != RAMEN_GAME))
   {
      rm->drag = 0;
      return;
   }
   if (!ramen_touch_world(rm, &deg, &dist))
      return;   /* at the pivot: hold still */
   while (deg - rm->drag_last > 180.0f)
      deg -= 360.0f;
   while (deg - rm->drag_last < -180.0f)
      deg += 360.0f;
   rm->drag_acc += deg - rm->drag_last;
   rm->drag_last = deg;
   if (fabsf(rm->drag_acc) > RAMEN_TOUCH_TAP_DEG)
      rm->drag_moved = true;
   if (!rm->drag_moved)
      return;

   w   = rm->drag_w0 + rm->drag_acc;
   vel = dt > 0.0f ? (w - (rm->drag == 1 ? rm->wheel : rm->gwheel)) / dt : 0.0f;
   rm->drag_vel += (vel - rm->drag_vel) * MIN(1.0f, dt / 0.05f);

   if (rm->drag == 1)
   {
      int slot = ramen_slot_at(w);
      if (slot != ramen_slot_at(rm->wheel_target))
         ramen_snd_play(SND_TICK);
      rm->wheel        = w;
      rm->wheel_vel    = rm->drag_vel;
      rm->wheel_target = ramen_slot_target(slot);
      rm->settled      = false;
      rm->label        = 0.0f;
   }
   else
   {
      rm->gwheel     = w;
      rm->gwheel_vel = rm->drag_vel;
      rm->gsettled   = false;
      /* past half a slot the ring steps, so the offset stays small and
       * the drawn ring stays whole */
      while (rm->gwheel - rm->gwheel_target > rm->gslot * 0.5f)
         ramen_game_turn(rm, 1);
      while (rm->gwheel - rm->gwheel_target < -rm->gslot * 0.5f)
         ramen_game_turn(rm, -1);
   }
}

static void ramen_touch_tap(ramen_handle_t *rm, int drag)
{
   float deg, dist;

   if (!ramen_touch_world(rm, &deg, &dist))
      return;
   if (fabsf(dist - RAMEN_TUBE_R) > RAMEN_SPHERE_R * RAMEN_TOUCH_BAND)
      return;
   if (drag == 1)
   {
      int k = (int)lroundf((deg - rm->wheel) / RAMEN_SLOT_DEG);

      if (fabsf(rm->wheel + k * RAMEN_SLOT_DEG - deg) > ramen_sphere_deg(1.15f))
         return;
      if (k == ramen_slot_at(rm->wheel_target))
         ramen_entry_action(rm, NULL, 0, MENU_ACTION_OK);
      else
      {
         rm->wheel_target = ramen_slot_target(k);
         rm->settled      = false;
         rm->label        = 0.0f;
         ramen_snd_play(SND_TICK);
      }
   }
   else
   {
      int n    = (int)ramen_game_count(rm, rm->system);
      int last = MIN(n, RAMEN_GAMES_MAX);
      float rel, hit;
      int i;

      /* the parked system sphere: back out */
      if (fabsf(deg - RAMEN_PARK_DEG) < ramen_sphere_deg(1.15f))
      {
         ramen_entry_action(rm, NULL, 0, MENU_ACTION_CANCEL);
         return;
      }
      if (n == 0)
         return;
      rel = RAMEN_GAME_DEG + (rm->gwheel - rm->gwheel_target) - deg;
      i   = (int)lroundf(rel / rm->gslot);
      hit = ramen_sphere_deg(RAMEN_GAME_SCALE * (i == 0 ? 1.15f : 1.0f));
      if (i < -last || i > 0 || fabsf(rel - i * rm->gslot) > hit)
         return;
      if (i == 0)
         ramen_entry_action(rm, NULL, 0, MENU_ACTION_OK);
      else
         ramen_game_turn(rm, i);
   }
}

static void ramen_touch_up(ramen_handle_t *rm)
{
   int drag = rm->drag;

   rm->drag = 0;
   if (!drag)
      return;
   if ((drag == 1 && rm->state != RAMEN_SYSTEM)
         || (drag == 2 && rm->state != RAMEN_GAME))
      return;
   if (!rm->drag_moved)
   {
      ramen_touch_tap(rm, drag);
      return;
   }
   /* the fling: where the velocity would carry it, in slots */
   if (drag == 1)
   {
      int cur = ramen_slot_at(rm->wheel);
      int k   = ramen_slot_at(rm->wheel + rm->drag_vel * RAMEN_TOUCH_FLING_S);

      k = MAX(cur - RAMEN_TOUCH_MAX_STEPS, MIN(cur + RAMEN_TOUCH_MAX_STEPS, k));
      if (k != ramen_slot_at(rm->wheel_target))
         ramen_snd_play(SND_TICK);
      rm->wheel_target = ramen_slot_target(k);
      rm->settled      = false;
      rm->label        = 0.0f;
   }
   else
   {
      /* fast enough: coast; otherwise the spring takes it from here,
       * landing on whichever slot the drag left it nearest */
      if (fabsf(rm->drag_vel) >= RAMEN_GAME_COAST_MIN)
      {
         rm->gcoast     = true;
         rm->gwheel_vel = MAX(-RAMEN_GAME_COAST_MAX,
               MIN(RAMEN_GAME_COAST_MAX, rm->drag_vel));
      }
      rm->gsettled = false;
   }
}

/* Drain the panel's events; a finger coming or going is handled on the
 * SYN that reports it. Any touch counts as input to RetroArch, and the
 * one that wakes the screensaver does nothing else. */
static void ramen_touch_poll(ramen_handle_t *rm)
{
   struct input_event ev[32];
   ssize_t n;

   if (rm->touch_fd < 0)
      return;
   while ((n = read(rm->touch_fd, ev, sizeof(ev))) > 0)
   {
      size_t i, cnt = (size_t)n / sizeof(ev[0]);

      for (i = 0; i < cnt; i++)
      {
         switch (ev[i].type)
         {
            case EV_ABS:
               if (ev[i].code == ABS_X)
                  rm->touch_raw_x = ev[i].value;
               else if (ev[i].code == ABS_Y)
                  rm->touch_raw_y = ev[i].value;
               break;
            case EV_KEY:
               if (ev[i].code == BTN_TOUCH)
                  rm->touch_raw_down = ev[i].value != 0;
               break;
            case EV_SYN:
               if (ev[i].code != SYN_REPORT)
                  break;
               rm->touch_x = (float)rm->touch_raw_x * (float)rm->cam_w
                           / (float)(rm->touch_max_x + 1);
               rm->touch_y = (float)rm->touch_raw_y * (float)rm->cam_h
                           / (float)(rm->touch_max_y + 1);
               if (rm->touch_raw_down || rm->touch_down)
                  menu_state_get_ptr()->input_last_time_us =
                     cpu_features_get_time_usec();
               if (rm->touch_raw_down == rm->touch_down)
                  break;
               rm->touch_down = rm->touch_raw_down;
               if (!rm->touch_down)
                  ramen_touch_up(rm);
               else if (rm->saver)
               {
                  struct menu_state *menu_st = menu_state_get_ptr();
                  menu_st->flags &= ~MENU_ST_FLAG_SCREENSAVER_ACTIVE;
                  ramen_saver(rm, false);
                  rm->drag = 0;
               }
               else
                  ramen_touch_down(rm);
               break;
            default:
               break;
         }
      }
   }
}

/* ---- the accelerometer ------------------------------------------------ */

static bool ramen_write_str(const char *path, const char *val)
{
   FILE *f = fopen(path, "w");

   if (!f)
      return false;
   fputs(val, f);
   return fclose(f) == 0;
}

static bool ramen_imu_write(ramen_handle_t *rm, const char *attr,
      const char *val)
{
   char path[160];

   snprintf(path, sizeof(path), "%s/%s", rm->imu_sysfs, attr);
   return ramen_write_str(path, val);
}

static void ramen_imu_open(ramen_handle_t *rm)
{
   int i;

   rm->imu_fd = -1;
   for (i = 0; i < 16; i++)
   {
      char path[160], name[64], hz[16];

      snprintf(rm->imu_sysfs, sizeof(rm->imu_sysfs),
            "/sys/bus/iio/devices/iio:device%d", i);
      snprintf(path, sizeof(path), "%s/name", rm->imu_sysfs);
      if (!ramen_read_line(path, name, sizeof(name))
            || !string_is_equal(name, RAMEN_IMU_NAME))
         continue;
      snprintf(hz, sizeof(hz), "%d", RAMEN_IMU_HZ);
      ramen_imu_write(rm, "buffer/enable", "0");
      if (!ramen_imu_write(rm, "scan_elements/in_accel_x_en", "1")
            || !ramen_imu_write(rm, "scan_elements/in_accel_y_en", "1")
            || !ramen_imu_write(rm, "scan_elements/in_accel_z_en", "1")
            || !ramen_imu_write(rm, "scan_elements/in_timestamp_en", "0")
            || !ramen_imu_write(rm, "buffer/length", "64")
            || !ramen_imu_write(rm, "in_accel_sampling_frequency", hz))
      {
         RARCH_WARN("[ramen] accelerometer: cannot set up %s\n", rm->imu_sysfs);
         break;
      }
      snprintf(path, sizeof(path), "/dev/iio:device%d", i);
      rm->imu_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (rm->imu_fd < 0)
         RARCH_WARN("[ramen] accelerometer: cannot open %s\n", path);
      else
         RARCH_LOG("[ramen] accelerometer: %s\n", path);
      return;
   }
   rm->imu_sysfs[0] = '\0';
}

/* Start or stop the stream: it runs on the hub, so it is off whenever
 * nothing would show it. */
static void ramen_imu_set(ramen_handle_t *rm, bool on)
{
   if (rm->imu_fd < 0 || rm->imu_on == on)
      return;
   ramen_imu_write(rm, "buffer/enable", on ? "1" : "0");
   rm->imu_on     = on;
   rm->imu_have_g = false;
}

static void ramen_imu_poll(ramen_handle_t *rm)
{
   int32_t rec[64][3];
   ssize_t n;
   float kick    = 0.0f;
   const float sdt = 1.0f / (float)RAMEN_IMU_HZ;
   float ang;

   if (rm->imu_fd < 0 || !rm->imu_on)
      return;
   /* the tube's tangent at the selected slot, screen y down */
   ang = (rm->state == RAMEN_GAME ? RAMEN_GAME_DEG : RAMEN_SELECT_DEG)
       * (float)M_PI / 180.0f;
   while ((n = read(rm->imu_fd, rec, sizeof(rec))) > 0)
   {
      size_t i, cnt = (size_t)n / sizeof(rec[0]);

      for (i = 0; i < cnt; i++)
      {
         float a[3], d[3], sx, sy, at;
         int k;

         for (k = 0; k < 3; k++)
            a[k] = (float)rec[i][k] * RAMEN_IMU_SCALE;
         if (!rm->imu_have_g)
         {
            memcpy(rm->imu_g, a, sizeof(a));
            rm->imu_have_g = true;
            continue;
         }
         for (k = 0; k < 3; k++)
         {
            rm->imu_g[k] += (a[k] - rm->imu_g[k]) * MIN(1.0f, sdt / RAMEN_IMU_G_TAU);
            d[k]          = a[k] - rm->imu_g[k];
         }
         sx = RAMEN_IMU_SIGN_X * d[RAMEN_IMU_AXIS_X];   /* screen right */
         sy = RAMEN_IMU_SIGN_Y * d[RAMEN_IMU_AXIS_Y];   /* screen up */
         /* along the tangent of increasing angle: (sin a, -cos a) with
          * y down, so up is -y */
         at = sx * sinf(ang) + sy * cosf(ang);
         if (fabsf(at) < RAMEN_IMU_DEAD)
            continue;
         at   -= (at > 0.0f ? RAMEN_IMU_DEAD : -RAMEN_IMU_DEAD);
         kick -= at * RAMEN_IMU_KICK * sdt;   /* the spheres lag behind */
      }
   }
   if (kick == 0.0f || rm->drag || rm->gcoast || rm->usb_req
         || rm->launch > 0.0f)
      return;
   if (rm->state == RAMEN_SYSTEM && !rm->boot_spin)
      rm->wheel_vel = MAX(-RAMEN_IMU_VMAX,
            MIN(RAMEN_IMU_VMAX, rm->wheel_vel + kick));
   else if (rm->state == RAMEN_GAME && rm->gintro >= rm->gintro_len)
      rm->gwheel_vel = MAX(-RAMEN_IMU_GVMAX,
            MIN(RAMEN_IMU_GVMAX, rm->gwheel_vel + kick * 0.6f));
}

/* The in-game page is drawn once the raster is open and a degauss has
 * settled. */
static bool ramen_ig_page_shown(ramen_handle_t *rm)
{
   return rm->state == RAMEN_INGAME && rm->ig_v >= 0.97f && !rm->ig_closing
      && !(rm->ig_degauss >= 0.0f && rm->ig_degauss < RAMEN_DEGAUSS_SHOW);
}

static void ramen_update(ramen_handle_t *rm, retro_time_t now)
{
   float dt;

   if (!rm->last_us)
      rm->last_us = now;
   dt          = (float)(now - rm->last_us) * 1e-6f;
   rm->last_us = now;
   if (dt > 0.05f)
      dt = 0.05f;
   rm->dt = dt;

   ramen_touch_poll(rm);
   ramen_drag_update(rm, dt);
   ramen_imu_poll(rm);

   if (rm->boot_spin)
   {
      int slot;

      rm->wheel_vel *= expf(-RAMEN_BOOT_FRICTION * dt);
      rm->wheel     += rm->wheel_vel * dt;
      /* a tick each time a sphere crosses a slot boundary */
      slot = (int)floorf((rm->wheel - rm->wheel_target) / RAMEN_SLOT_DEG);
      if (slot != rm->boot_slot)
      {
         rm->boot_slot = slot;
         ramen_snd_play(SND_TICK);
      }
      if (rm->wheel_vel < 75.0f)
         rm->boot_spin = false;
   }
   else if (rm->drag != 1)
   {
      ramen_spring(&rm->wheel, &rm->wheel_vel, rm->wheel_target,
            RAMEN_SPRING_HZ, RAMEN_SPRING_ZETA, dt);
      if (fabsf(rm->wheel_target - rm->wheel) < 0.3f
            && fabsf(rm->wheel_vel) < 8.0f && !rm->settled)
      {
         rm->settled   = true;
         rm->wheel     = rm->wheel_target;
         rm->wheel_vel = 0.0f;
      }
   }

   /* Labels surface once settled (~250 ms) and sink fast otherwise. */
   if (rm->settled && rm->state == RAMEN_SYSTEM)
      rm->label = MIN(1.0f, rm->label + dt / 0.25f);
   else
      rm->label = MAX(0.0f, rm->label - dt / 0.12f);

   if (rm->jiggle > 0.0f)
      rm->jiggle = MAX(0.0f, rm->jiggle - dt / 0.2f);

   /* The launch sequence and its reverse. */
   if (rm->state == RAMEN_LAUNCHING)
   {
      rm->seq += dt / RAMEN_SEQ_TOTAL;
      if (rm->seq >= 1.0f)
      {
         rm->seq    = 1.0f;
         rm->state  = RAMEN_GAME;
         rm->gintro = 0.0f;
      }
   }
   else if (rm->state == RAMEN_RETURNING)
   {
      /* The games fall out first; the sphere leaves once they are gone. */
      if (rm->goutro < RAMEN_OUTRO_S)
         rm->goutro += dt;
      else
      {
         rm->seq -= dt / RAMEN_SEQ_TOTAL;
         if (rm->seq <= 0.0f)
         {
            rm->seq   = 0.0f;
            rm->state = RAMEN_SYSTEM;
            rm->settled = false;  /* lets the label resurface */
         }
      }
   }

   if (rm->state == RAMEN_GAME || rm->state == RAMEN_RETURNING
         || rm->state == RAMEN_STARTING)
      rm->gintro += dt;

   /* The game queue: coasting after a flick, else its own detent spring. */
   if (rm->gcoast)
   {
      rm->gwheel_vel *= expf(-RAMEN_GAME_FRICTION * dt);
      rm->gwheel     += rm->gwheel_vel * dt;
      while (rm->gwheel - rm->gwheel_target > rm->gslot * 0.5f)
         ramen_game_turn(rm, 1);
      while (rm->gwheel - rm->gwheel_target < -rm->gslot * 0.5f)
         ramen_game_turn(rm, -1);
      if (fabsf(rm->gwheel_vel) < RAMEN_GAME_COAST_MIN)
         rm->gcoast = false;
   }
   else if (rm->drag != 2)
      ramen_spring(&rm->gwheel, &rm->gwheel_vel, rm->gwheel_target,
            RAMEN_SPRING_HZ, RAMEN_SPRING_ZETA, dt);
   if (fabsf(rm->gwheel_target - rm->gwheel) < 0.3f
         && fabsf(rm->gwheel_vel) < 8.0f && !rm->gsettled)
   {
      rm->gsettled   = true;
      rm->gwheel     = rm->gwheel_target;
      rm->gwheel_vel = 0.0f;
   }

   /* The monitor comes on once the sphere has parked and goes off as
    * the games start falling out. */
   /* The monitor is out of the picture under the in-game menu: leave it
    * (and its cues) alone there. */
   if (rm->state != RAMEN_INGAME)
   {
      float was = rm->mon;
      if (rm->state == RAMEN_GAME || rm->state == RAMEN_STARTING)
         rm->mon = MIN(1.0f, rm->mon + dt / RAMEN_MON_ON_S);
      else
         rm->mon = MAX(0.0f, rm->mon - dt / RAMEN_MON_OFF_S);
      if (was <= 0.0f && rm->mon > 0.0f)
         ramen_snd_play(SND_POWER);
      else if (was >= 1.0f && rm->mon < 1.0f)
         ramen_snd_play(SND_OFF);
   }

   if (rm->notice_t > 0.0f)
      rm->notice_t -= dt;

   /* The vials: up on the carousel and the in-game page, polled while up,
    * and read afresh each time they come back. They make way for the
    * carousel's notice and the USB screen, which use the same corner. */
   rm->clock = fmodf(rm->clock + dt, 1000.0f);
   if ((rm->state == RAMEN_SYSTEM && rm->notice_t <= 0.0f)
         || ramen_ig_page_shown(rm))
   {
      if (rm->hud <= 0.0f)
         rm->status_poll = 0.0f;
      rm->hud = MIN(1.0f, rm->hud + dt / RAMEN_HUD_S);
   }
   else
      rm->hud = MAX(0.0f, rm->hud - dt / RAMEN_HUD_S);
   if (rm->hud > 0.0f)
   {
      rm->status_poll -= dt;
      if (rm->status_poll <= 0.0f)
      {
         rm->status_poll = RAMEN_STATUS_POLL_S;
         ramen_read_status(rm);
      }
   }

   /* Storage Mode: once the host ejects, the gadget script remounts
    * /data, and render() takes it from there. */
   if (rm->state == RAMEN_USB)
   {
      rm->usb_poll += dt;
      if (rm->usb_poll >= RAMEN_USB_POLL_S)
      {
         rm->usb_poll   = 0.0f;
         rm->sd_present = ramen_sd_present();
         if (!rm->usb_req && ramen_mounted("/data"))
            rm->usb_req = 3;
      }
   }

   if (rm->state == RAMEN_INGAME)
   {
      /* the scroll starts over whenever the cursor or the page moves */
      if (rm->ig_scroll_page != (int)rm->ig_page
            || rm->ig_scroll_row != rm->ig_sel[rm->ig_page])
      {
         rm->ig_scroll_page = (int)rm->ig_page;
         rm->ig_scroll_row  = rm->ig_sel[rm->ig_page];
         rm->ig_scroll_t    = 0.0f;
      }
      else
         rm->ig_scroll_t += dt;
      if (rm->ig_degauss >= 0.0f)
         rm->ig_degauss += dt;
      if (!rm->ig_closing)
         rm->ig_v = MIN(1.0f, rm->ig_v + dt / RAMEN_IG_OPEN_S);
      else if (rm->ig_v > 0.0f)
         rm->ig_v = MAX(0.0f, rm->ig_v - dt / RAMEN_IG_VCLOSE_S);
      else
         rm->ig_h = MAX(0.0f, rm->ig_h - dt / RAMEN_IG_HCLOSE_S);
   }

   if (rm->state == RAMEN_STARTING)
   {
      rm->launch = MIN(1.0f, rm->launch + dt / RAMEN_LAUNCH_S);
      if (rm->launch >= RAMEN_LAUNCH_FADE)
         rm->fade = MAX(0.0f, rm->fade - dt / RAMEN_FADE_S);
   }
   else
   {
      rm->fade = MIN(1.0f, rm->fade + dt / RAMEN_FADE_S);
      /* back from a game: once the picture is up, pull out of the glass */
      if (rm->launch > 0.0f && rm->fade >= 1.0f)
         rm->launch = MAX(0.0f, rm->launch - dt / RAMEN_LAUNCH_S);
   }
}

/* Where the launched sphere is along the tube for a point in the sequence:
 * it holds through the clear-out, pulls back a little, then runs from the
 * selected slot to the parking spot. */
static float ramen_launch_angle(float seq)
{
   float t = seq * RAMEN_SEQ_TOTAL;

   if (t <= RAMEN_SEQ_CLEAR)
      return RAMEN_SELECT_DEG;
   t -= RAMEN_SEQ_CLEAR;
   if (t <= RAMEN_SEQ_ANTIC)
      return RAMEN_SELECT_DEG - 6.0f * ramen_smooth(t / RAMEN_SEQ_ANTIC);
   t -= RAMEN_SEQ_ANTIC;
   /* ease-in, then a soft landing at the parking spot */
   return RAMEN_SELECT_DEG - 6.0f
        + (RAMEN_PARK_DEG - RAMEN_SELECT_DEG + 6.0f)
          * ramen_smooth(t / RAMEN_SEQ_RUN);
}

/* How far the neighbours have retracted toward their edges, 0..1. */
static float ramen_clearout(float seq)
{
   return ramen_smooth(seq * RAMEN_SEQ_TOTAL / RAMEN_SEQ_CLEAR);
}

/* ---- the in-game menu ------------------------------------------------ */

static core_option_manager_t *ramen_ig_options(void)
{
   runloop_state_t *rs = runloop_state_get_ptr();
   return rs ? rs->core_options : NULL;
}

/* The core option behind a Core Settings row (row 0 is Control Remaps),
 * skipping the ones the core hides. -1 if there is none. */
static int ramen_ig_option_at(int row)
{
   core_option_manager_t *opts = ramen_ig_options();
   size_t i;
   int seen = 0;

   if (!opts || row <= 0)
      return -1;
   for (i = 0; i < opts->size; i++)
   {
      if (!core_option_manager_get_visible(opts, i))
         continue;
      if (++seen == row)
         return (int)i;
   }
   return -1;
}

static int ramen_ig_count(ramen_handle_t *rm)
{
   switch (rm->ig_page)
   {
      case IG_TOP:
         return IG_TOP_COUNT;
      case IG_OPTIONS:
      {
         core_option_manager_t *opts = ramen_ig_options();
         size_t i;
         int n = 1;
         if (opts)
            for (i = 0; i < opts->size; i++)
               if (core_option_manager_get_visible(opts, i))
                  n++;
         return n;
      }
      case IG_REMAPS:
         return 1 + RAMEN_IG_BINDS;   /* the d-pad mode, then the binds */
      default:
         return 0;
   }
}

/* What a remap target reads as: the core's own name for it when it gives
 * one, the RetroPad name otherwise. */
static const char *ramen_ig_remap_name(unsigned id)
{
   settings_t *settings = config_get_ptr();
   rarch_system_info_t *sys = &runloop_state_get_ptr()->system;
   unsigned port = settings ? settings->uints.input_remap_ports[0] : 0;
   const char *desc;

   if (id == RARCH_UNMAPPED)
      return "---";
   if (id >= RARCH_FIRST_META_KEY)
      return "?";
   desc = sys->input_desc_btn[port][id];
   if (!string_is_empty(desc))
      return desc;
   return input_config_bind_map_get_desc(id);
}

/* Whether the core names any of its inputs; if not, nothing is skipped. */
static bool ramen_ig_core_has_descs(void)
{
   settings_t *settings = config_get_ptr();
   rarch_system_info_t *sys = &runloop_state_get_ptr()->system;
   unsigned port = settings ? settings->uints.input_remap_ports[0] : 0;
   unsigned i;

   for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
      if (!string_is_empty(sys->input_desc_btn[port][i]))
         return true;
   return false;
}

/* The Control Remaps page's first row: what the d-pad drives, the d-pad
 * itself or one of the analog sticks (full deflection), by remapping its
 * four directions to the stick's half-axes. The pad has no sticks of its
 * own, so this is how analog-only inputs get reached. */
enum
{
   IG_DPAD_DPAD = 0,
   IG_DPAD_LEFT_STICK,
   IG_DPAD_RIGHT_STICK,
   IG_DPAD_MODES
};

static const unsigned ramen_ig_dpad_ids[4] = {
   RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
   RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT };

static const unsigned ramen_ig_dpad_targets[IG_DPAD_MODES][4] = {
   { RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
     RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT },
   { RARCH_ANALOG_LEFT_Y_MINUS, RARCH_ANALOG_LEFT_Y_PLUS,
     RARCH_ANALOG_LEFT_X_MINUS, RARCH_ANALOG_LEFT_X_PLUS },
   { RARCH_ANALOG_RIGHT_Y_MINUS, RARCH_ANALOG_RIGHT_Y_PLUS,
     RARCH_ANALOG_RIGHT_X_MINUS, RARCH_ANALOG_RIGHT_X_PLUS } };

/* The mode the d-pad's remaps add up to, or -1 if they were set by hand. */
static int ramen_ig_dpad_mode(const unsigned *ids)
{
   int m, d;

   for (m = 0; m < IG_DPAD_MODES; m++)
   {
      for (d = 0; d < 4; d++)
         if (ids[ramen_ig_dpad_ids[d]] != ramen_ig_dpad_targets[m][d])
            break;
      if (d == 4)
         return m;
   }
   return -1;
}

/* Remaps go where RetroArch will look for them at the next load: the
 * game's own file (remaps/<core>/<game>.rmp) once the d-pad mode has been
 * set for it, or whenever that file already exists, since RetroArch then
 * prefers it; the core's file (remaps/<core>/<core>.rmp) otherwise. */
static void ramen_ig_save_remaps(ramen_handle_t *rm)
{
   settings_t *settings = config_get_ptr();
   const char *core     = runloop_state_get_ptr()->system.info.library_name;
   const char *content  = path_get(RARCH_PATH_BASENAME);
   char dir[PATH_MAX_LENGTH];
   char path[PATH_MAX_LENGTH];
   char game[PATH_MAX_LENGTH];

   if (!rm->ig_remaps_dirty || !settings || string_is_empty(core))
      return;
   rm->ig_remaps_dirty = false;
   fill_pathname_join_special(dir, settings->paths.directory_input_remapping,
         core, sizeof(dir));
   if (!path_is_directory(dir))
      path_mkdir(dir);
   fill_pathname_join_special_ext(path,
         settings->paths.directory_input_remapping, core, core,
         FILE_PATH_REMAP_EXTENSION, sizeof(path));
   if (!string_is_empty(content))
   {
      fill_pathname_join_special_ext(game,
            settings->paths.directory_input_remapping, core,
            path_basename_nocompression(content),
            FILE_PATH_REMAP_EXTENSION, sizeof(game));
      if (rm->ig_remaps_game || path_is_valid(game))
      {
         strlcpy(path, game, sizeof(path));
         retroarch_ctl(RARCH_CTL_SET_REMAPS_GAME_ACTIVE, NULL);
      }
   }
   rm->ig_remaps_game = false;
   if (input_remapping_save_file(path))
      RARCH_LOG("[ramen] remaps saved to %s\n", path);
   else
      RARCH_WARN("[ramen] saving remaps to %s failed\n", path);
}

static void ramen_ig_goto(ramen_handle_t *rm, enum ramen_ig_page page)
{
   if (rm->ig_page == IG_REMAPS)
      ramen_ig_save_remaps(rm);
   rm->ig_page    = page;
   rm->ig_degauss = 0.0f;
   ramen_snd_play(SND_THUNK);
}

static void ramen_ig_close(ramen_handle_t *rm, enum ramen_ig_cmd cmd)
{
   if (rm->ig_page == IG_REMAPS)
      ramen_ig_save_remaps(rm);
   rm->ig_closing = true;
   rm->ig_cmd     = cmd;
   ramen_snd_play(SND_OFF);
}

/* Left/Right on a row: the state slot, a core option, or a remap. */
static void ramen_ig_adjust(ramen_handle_t *rm, int dir)
{
   settings_t *settings = config_get_ptr();
   int sel = rm->ig_sel[rm->ig_page];

   switch (rm->ig_page)
   {
      case IG_TOP:
         if ((sel == IG_TOP_SAVE || sel == IG_TOP_LOAD) && settings)
         {
            int slot = settings->ints.state_slot + dir;
            settings->ints.state_slot = MAX(0, MIN(99, slot));
         }
         break;
      case IG_OPTIONS:
      {
         int idx = ramen_ig_option_at(sel);
         if (idx >= 0)
            core_option_manager_adjust_val(ramen_ig_options(), (size_t)idx,
                  dir, false);
         break;
      }
      case IG_REMAPS:
      {
         /* cycle through the bind order, with unmapped at the end,
          * skipping inputs the core does not use */
         unsigned *ids = settings->uints.input_remap_ids[0];
         unsigned cur;
         bool has_descs;
         int pos = RAMEN_IG_BINDS, i, tries;

         if (sel == 0)
         {
            /* the d-pad mode; a hand-made mix starts over from the d-pad */
            int m = ramen_ig_dpad_mode(ids), d;
            m = (m < 0) ? (dir > 0 ? IG_DPAD_LEFT_STICK : IG_DPAD_RIGHT_STICK)
                        : (m + dir + IG_DPAD_MODES) % IG_DPAD_MODES;
            for (d = 0; d < 4; d++)
               ids[ramen_ig_dpad_ids[d]] = ramen_ig_dpad_targets[m][d];
            rm->ig_remaps_dirty = true;
            rm->ig_remaps_game  = true;   /* a per-game choice */
            break;
         }
         sel--;   /* row 1 is bind 0 */
         cur       = ids[sel];
         has_descs = ramen_ig_core_has_descs();
         for (i = 0; i < RAMEN_IG_BINDS; i++)
            if (input_config_bind_order[i] == cur)
            {
               pos = i;
               break;
            }
         for (tries = 0; tries <= RAMEN_IG_BINDS; tries++)
         {
            pos = (pos + dir + RAMEN_IG_BINDS + 1) % (RAMEN_IG_BINDS + 1);
            if (pos == RAMEN_IG_BINDS)
               break;
            if (!has_descs || input_config_bind_order[pos] >= RARCH_FIRST_CUSTOM_BIND
                  || !string_is_empty(runloop_state_get_ptr()->system
                     .input_desc_btn[settings->uints.input_remap_ports[0]]
                     [input_config_bind_order[pos]]))
               break;
         }
         ids[sel] = (pos == RAMEN_IG_BINDS) ? RARCH_UNMAPPED
                  : input_config_bind_order[pos];
         rm->ig_remaps_dirty = true;
         break;
      }
      default:
         break;
   }
}

static void ramen_ig_activate(ramen_handle_t *rm)
{
   int sel = rm->ig_sel[rm->ig_page];

   switch (rm->ig_page)
   {
      case IG_TOP:
         switch (sel)
         {
            case IG_TOP_RESUME:   ramen_ig_close(rm, IG_CMD_RESUME); break;
            case IG_TOP_SAVE:     ramen_ig_close(rm, IG_CMD_SAVE);   break;
            case IG_TOP_LOAD:     ramen_ig_close(rm, IG_CMD_LOAD);   break;
            case IG_TOP_SETTINGS: ramen_ig_goto(rm, IG_OPTIONS);     break;
            case IG_TOP_EXIT:     ramen_ig_close(rm, IG_CMD_EXIT);   break;
         }
         break;
      case IG_OPTIONS:
         if (sel == 0)
            ramen_ig_goto(rm, IG_REMAPS);
         else
            ramen_ig_adjust(rm, 1);
         break;
      case IG_REMAPS:
         ramen_ig_adjust(rm, 1);
         break;
      default:
         break;
   }
}

/* A row's label and value. */
static void ramen_ig_row(ramen_handle_t *rm, int row, char *label,
      size_t llen, char *value, size_t vlen)
{
   settings_t *settings = config_get_ptr();

   label[0] = value[0] = '\0';
   switch (rm->ig_page)
   {
      case IG_TOP:
         switch (row)
         {
            case IG_TOP_RESUME:   strlcpy(label, "Resume Game", llen); break;
            case IG_TOP_SAVE:     strlcpy(label, "Save State", llen);  break;
            case IG_TOP_LOAD:     strlcpy(label, "Load State", llen);  break;
            case IG_TOP_SETTINGS: strlcpy(label, "Core Settings", llen); break;
            case IG_TOP_EXIT:     strlcpy(label, "Exit Game", llen);   break;
         }
         if ((row == IG_TOP_SAVE || row == IG_TOP_LOAD) && settings)
            snprintf(value, vlen, "Slot %d", settings->ints.state_slot);
         break;
      case IG_OPTIONS:
         if (row == 0)
            strlcpy(label, "Control Remaps", llen);
         else
         {
            int idx = ramen_ig_option_at(row);
            core_option_manager_t *opts = ramen_ig_options();
            const char *d, *v;
            if (idx < 0 || !opts)
               break;
            d = core_option_manager_get_desc(opts, (size_t)idx, false);
            v = core_option_manager_get_val_label(opts, (size_t)idx);
            strlcpy(label, d ? d : "?", llen);
            strlcpy(value, v ? v : "", vlen);
         }
         break;
      case IG_REMAPS:
         if (settings && row == 0)
         {
            static const char *modes[IG_DPAD_MODES] = {
               "D-Pad", "Left Stick", "Right Stick" };
            int m = ramen_ig_dpad_mode(settings->uints.input_remap_ids[0]);
            strlcpy(label, "D-Pad Drives", llen);
            strlcpy(value, m < 0 ? "Custom" : modes[m], vlen);
         }
         else if (settings && row <= RAMEN_IG_BINDS)
         {
            strlcpy(label, input_config_bind_map_get_desc((unsigned)row - 1), llen);
            strlcpy(value, ramen_ig_remap_name(
                     settings->uints.input_remap_ids[0][row - 1]), vlen);
         }
         break;
      default:
         break;
   }
}

static const char *ramen_ig_title(ramen_handle_t *rm, char *buf, size_t len)
{
   const char *path;

   switch (rm->ig_page)
   {
      case IG_OPTIONS: return "Core Settings";
      case IG_REMAPS:  return "Control Remaps";
      default: break;
   }
   path = ramen_game_path(rm, rm->system, rm->game);
   if (path)
      ramen_game_title(path, buf, len);
   else
      strlcpy(buf, "Paused", len);
   return buf;
}

/* ---- launching a game ------------------------------------------------ */

/* Back from a game: fade in on the monitor's glass and pull out. */
static void ramen_return_from_game(ramen_handle_t *rm)
{
   rm->state    = RAMEN_GAME;
   rm->started  = false;
   rm->seq      = 1.0f;
   rm->mon      = 1.0f;
   rm->gintro   = 100.0f;
   rm->goutro   = 0.0f;
   rm->snap_cam = true;
   rm->fade     = 0.0f;
   rm->launch   = 1.0f;
}

static void ramen_start_game(ramen_handle_t *rm)
{
   content_ctx_info_t info;
   const char *core = rm->sys[rm->system].core;
   const char *rom  = ramen_game_path(rm, rm->system, rm->game);

   if (!rom)
      return;
   memset(&info, 0, sizeof(info));
   RARCH_LOG("[ramen] launching %s with %s\n", rom, core);
   ramen_save_last_system(rm, rm->system);
   /* RetroArch closes the menu itself once the content is up. */
   if (!task_push_load_content_with_new_core_from_menu(core, rom, &info,
            CORE_TYPE_PLAIN, NULL, NULL))
   {
      RARCH_ERR("[ramen] launch failed\n");
      rm->state   = RAMEN_GAME;
      rm->started = false;
      rm->launch  = 0.0f;
   }
}

/* ---- menu driver ----------------------------------------------------- */

static void *ramen_init(void **userdata, bool video_is_threaded)
{
   ramen_handle_t *rm   = NULL;
   menu_handle_t *menu  = (menu_handle_t*)calloc(1, sizeof(*menu));

   if (!menu)
      return NULL;
   if (!(rm = (ramen_handle_t*)calloc(1, sizeof(*rm))))
   {
      free(menu);
      return NULL;
   }
   *userdata = rm;
   rm->zoom  = 1.0f;
   rm->fade  = 1.0f;
   rm->meta_sys = rm->meta_game = -1;
   rm->ig_degauss = -1.0f;
   rm->ig_h       = 1.0f;
   rm->bat_pct    = -1;
   rm->vol        = -1;
   ramen_touch_open(rm);
   ramen_imu_open(rm);
   /* A config from before the links still names /data/bios. */
   {
      settings_t *settings = config_get_ptr();
      if (settings && !string_is_equal(settings->paths.directory_system,
               RAMEN_BIOS_LINKS))
         configuration_set_string(settings, settings->paths.directory_system,
               RAMEN_BIOS_LINKS);
   }
   ramen_link_bios();
   ramen_scan_systems(rm);
   ramen_begin_boot_spin(rm, ramen_load_last_system(rm));
   ramen_snd_load();   /* audio is up before the menu driver */
   ramen_art_start(rm);
   return menu;
}

/* RetroArch frees the handle itself after this; only its contents go here. */
static void ramen_saver(ramen_handle_t *rm, bool on);

static void ramen_free(void *data)
{
   ramen_handle_t *rm = (ramen_handle_t*)data;
   int i;

   if (!rm)
      return;
   ramen_saver(rm, false);
   ramen_imu_set(rm, false);
   if (rm->imu_fd >= 0)
      close(rm->imu_fd);
   if (rm->touch_fd >= 0)
      close(rm->touch_fd);
   rm->imu_fd = rm->touch_fd = -1;
   ramen_art_stop(rm);
   if (rm->font)
      font_driver_free(rm->font);
   rm->font = NULL;
   for (i = 0; i < RAMEN_SYSTEMS_MAX; i++)
   {
      if (rm->games[i])
         string_list_free(rm->games[i]);
      rm->games[i] = NULL;
   }
}

static void ramen_context_reset(void *data, bool video_is_threaded)
{
   ramen_handle_t *rm    = (ramen_handle_t*)data;
   gfx_display_t *p_disp = disp_get_ptr();
   unsigned w = 0, h = 0;

   if (!rm)
      return;

   video_driver_get_size(&w, &h);
   rm->font_size = (float)MIN(w, h) * 0.07f;
   rm->font      = gfx_display_font_file(p_disp, (char*)RAMEN_FONT_PATH,
         rm->font_size, video_is_threaded);
   if (!rm->font)
      RARCH_WARN("[ramen] could not load %s\n", RAMEN_FONT_PATH);

   if (!ramen_gl_init(rm))
      RARCH_ERR("[ramen] GL setup failed, nothing will be drawn\n");
   rm->last_us = 0;
}

static void ramen_context_destroy(void *data)
{
   ramen_handle_t *rm = (ramen_handle_t*)data;
   if (!rm)
      return;
   if (rm->font)
      font_driver_free(rm->font);
   rm->font = NULL;
   ramen_gl_free(rm);
}

/* Where the vials go: their centres and where each one's text starts.
 * In the carousel the volume vial sits under the battery; on the in-game
 * page they share the bottom line, the volume clear of the widest battery
 * text so that it never shifts. Returns the top of the pair. */
static float ramen_hud_layout(ramen_handle_t *rm, unsigned width,
      unsigned height, float vx[2], float vy[2], float tx[2], float sc)
{
   float S    = (float)MIN(width, height);
   float half = (RAMEN_VIAL_HH + 1.85f * RAMEN_VIAL_CAP) * S;
   float cw   = RAMEN_VIAL_HW * S * 1.22f;
   float gap  = 0.02f * S;

   if (rm->state == RAMEN_INGAME)
   {
      float bw = rm->font
         ? (float)font_driver_get_message_width(rm->font, "100%", 4, sc)
         : 0.2f * S;
      vy[0] = vy[1] = height - 0.03f * S - half;
      vx[0] = width * 0.10f + cw;
      tx[0] = vx[0] + cw + gap;
      vx[1] = tx[0] + bw + 0.05f * S + cw;
      tx[1] = vx[1] + cw + gap;
   }
   else
   {
      vx[0] = vx[1] = 0.05f * S + cw;
      tx[0] = tx[1] = vx[0] + cw + gap;
      vy[0] = 0.04f * S + half;
      vy[1] = vy[0] + 2.0f * half + 0.03f * S;
   }
   return vy[0] - half;
}

static void ramen_frame(void *data, video_frame_info_t *video_info)
{
   ramen_handle_t *rm   = (ramen_handle_t*)data;
   unsigned width       = video_info->width;
   unsigned height      = video_info->height;
   float S              = (float)MIN(width, height);
   float pivot[2]       = { RAMEN_PIVOT_X * width, RAMEN_PIVOT_Y * height };
   float radius         = RAMEN_TUBE_R * S;
   float spheres[RAMEN_MAX_SPHERES][4];
   float colors[RAMEN_MAX_SPHERES][3];
   float alphas[RAMEN_MAX_SPHERES];
   float arts[RAMEN_MAX_SPHERES];
   int count            = 0;
   int selected_slot;
   float clear, launch_deg, cam_target, dt;
   float mon_rect[4];
   float ps[2];          /* the pivot on screen */
   float zoomv;          /* the zoom in effect, dive included */
   bool in_sequence;
   float hud_vx[2], hud_vy[2], hud_tx[2], hud_top;
   const float hud_sc = RAMEN_HUD_TEXT;
   int k;

   if (!rm || !rm->gl_ready)
      return;

   ramen_update(rm, cpu_features_get_time_usec());
   ramen_art_drain(rm);
   dt            = rm->dt;
   mon_rect[0]   = pivot[0] + RAMEN_MON_X * S;
   mon_rect[1]   = pivot[1] - RAMEN_MON_Y * S;
   mon_rect[2]   = RAMEN_MON_W * S;
   mon_rect[3]   = RAMEN_MON_H * S;
   selected_slot = ramen_slot_at(rm->wheel_target);
   in_sequence   = rm->state != RAMEN_SYSTEM;
   clear         = in_sequence ? ramen_clearout(rm->seq) : 0.0f;
   launch_deg    = ramen_launch_angle(rm->seq);

   /* The camera chases the sphere: its target follows the sphere's run
    * through the tube, and it zooms out as it arrives. */
   cam_target = S * ramen_smooth((launch_deg - 60.0f) / (RAMEN_PARK_DEG - 60.0f));
   if (rm->snap_cam)
   {
      rm->cam      = cam_target;
      rm->cam_vel  = 0.0f;
      rm->snap_cam = false;
   }
   ramen_spring(&rm->cam, &rm->cam_vel, cam_target, RAMEN_CAM_HZ,
         RAMEN_CAM_ZETA, dt);
   rm->zoom = 1.0f - (1.0f - RAMEN_GAME_ZOOM)
            * ramen_smooth((launch_deg - 100.0f) / (RAMEN_PARK_DEG - 100.0f));
   ps[0]  = pivot[0] - rm->cam;
   ps[1]  = pivot[1];
   zoomv  = rm->zoom;
   if (rm->launch > 0.0f)
   {
      /* the dive: zoom about the monitor's centre until it fills the
       * screen, blending from the game view's camera */
      float l   = ramen_smooth(rm->launch);
      float mcx = mon_rect[0] + mon_rect[2] * 0.5f;
      float mcy = mon_rect[1] + mon_rect[3] * 0.5f;
      float tx  = width * 0.5f - (mcx - pivot[0]) * RAMEN_LAUNCH_ZOOM;
      float ty  = height * 0.5f - (mcy - pivot[1]) * RAMEN_LAUNCH_ZOOM;
      ps[0]  += (tx - ps[0]) * l;
      ps[1]  += (ty - ps[1]) * l;
      zoomv  += (RAMEN_LAUNCH_ZOOM - zoomv) * l;
   }
   rm->cam_ps[0]    = ps[0];
   rm->cam_ps[1]    = ps[1];
   rm->cam_zoom     = zoomv;
   rm->cam_pivot[0] = pivot[0];
   rm->cam_pivot[1] = pivot[1];
   rm->cam_s        = S;
   rm->cam_w        = width;
   rm->cam_h        = height;
   /* the accelerometer only while a wheel is on screen to show it */
   ramen_imu_set(rm, !rm->saver
         && (rm->state == RAMEN_SYSTEM || rm->state == RAMEN_GAME
             || rm->state == RAMEN_LAUNCHING || rm->state == RAMEN_RETURNING));

   /* The system spheres. In a sequence the selected one is driven by the
    * timeline and the neighbours retract past their edges. */
   for (k = (int)ceilf((-20.0f - rm->wheel) / RAMEN_SLOT_DEG);
        rm->wheel + k * RAMEN_SLOT_DEG <= 90.0f + 20.0f
        && count < RAMEN_MAX_SPHERES; k++)
   {
      float deg = rm->wheel + k * RAMEN_SLOT_DEG;
      float stretch, dim, scale;
      bool selected = (k == selected_slot);

      if (in_sequence)
      {
         if (selected)
            deg = launch_deg;
         else if (deg < RAMEN_SELECT_DEG)
            deg -= 40.0f * clear;
         else
            deg += 40.0f * clear;
         if (!selected && clear >= 1.0f)
            continue;
      }

      /* Motion blur: stretch along the tangent with angular speed. */
      stretch = MIN(3.0f, fabsf(rm->wheel_vel) / 400.0f);
      /* Selected slot grows and the rest dim, fading in as it settles. */
      dim     = selected ? 0.0f : 0.6f * MAX(rm->label, clear);
      scale   = 1.0f + (selected ? 0.15f * MAX(rm->label, in_sequence ? 1.0f : 0.0f) : 0.0f);
      if (selected && rm->jiggle > 0.0f)
      {
         /* squash-and-stretch along the tangent, one damped cycle */
         float j = sinf(rm->jiggle * (float)M_PI * 2.0f) * rm->jiggle * 0.25f;
         stretch += j;
         scale   -= j * 0.5f;
      }

      spheres[count][0] = deg * (float)M_PI / 180.0f;
      spheres[count][1] = scale;
      spheres[count][2] = stretch;
      spheres[count][3] = dim;
      memcpy(colors[count], rm->sys[ramen_slot_system(rm, k)].rgb,
            sizeof(colors[count]));
      alphas[count] = 1.0f;
      arts[count]   = -1.0f;
      count++;
   }

   /* The games, once the sphere has parked: a ring of n identical spheres
    * drawn relative to the selection, which is bookkeeping on the side.
    * Relative index i sits at RAMEN_GAME_DEG - i * slot, so the selected
    * sphere is on the diagonal and the rest trail clockwise toward the
    * bottom edge. In the intro each one bursts out of the sphere to its
    * slot in turn, the selected one first. A step turns the ring: one
    * sphere enters from the system sphere (i = 1) while the last one
    * (i = -n) leaves past the edge, and those two only exist while the
    * ring is in motion. In the outro the whole ring falls out of the
    * bottom, and nothing comes in behind it. */
   if (rm->state == RAMEN_GAME || rm->state == RAMEN_STARTING
         || (rm->state == RAMEN_RETURNING && rm->goutro < RAMEN_OUTRO_S))
   {
      int n        = (int)ramen_game_count(rm, rm->system);
      int last     = MIN(n, RAMEN_GAMES_MAX);
      float offset = rm->gwheel - rm->gwheel_target;
      float moving = MIN(1.0f, fabsf(offset) / rm->gslot);
      float emerge = RAMEN_PARK_DEG + RAMEN_GAME_EMERGE;
      bool leaving = rm->state == RAMEN_RETURNING;
      float fall   = 0.0f;
      int i;

      if (leaving)
      {
         /* gravity: ease-in, so it picks up speed on the way down */
         float t = MIN(1.0f, rm->goutro / RAMEN_OUTRO_S);
         fall    = RAMEN_OUTRO_DEG * t * t;
      }

      for (i = -last; i <= 1 && count < RAMEN_MAX_SPHERES; i++)
      {
         float rest = RAMEN_GAME_DEG + offset - i * rm->gslot;
         float deg  = rest;
         float a;

         if (n <= 0)
            continue;
         if (leaving && i == 1)
            continue;
         if (i <= 0)
         {
            /* the burst, staggered by how far down the list it sits */
            float t = (rm->gintro + i * RAMEN_INTRO_STAGGER)
                    / RAMEN_INTRO_BURST;
            deg = RAMEN_INTRO_START
                + (rest - RAMEN_INTRO_START) * ramen_burst(t);
         }
         deg += fall;
         if (deg < emerge || deg > RAMEN_GAME_END)
            continue;
         /* surfacing out of the sphere, sinking past the edge */
         a = ramen_smooth((deg - emerge) / RAMEN_GAME_FADE)
           * ramen_smooth((RAMEN_GAME_END - deg) / RAMEN_GAME_FADE);
         if (i == 1 || i == -last)
            a *= moving;
         if (a <= 0.0f)
            continue;
         {
            /* the game this sphere is, and its art if it has arrived;
             * ramen_art_prefetch asks for what is missing */
            int idx   = ((rm->game + i) % n + n) % n;
            int layer = ramen_art_find(rm, rm->system, idx);
            arts[count] = (layer >= 0 && rm->art[layer].present)
                        ? (float)layer : -1.0f;
         }
         alphas[count]     = a;
         spheres[count][0] = deg * (float)M_PI / 180.0f;
         spheres[count][1] = RAMEN_GAME_SCALE * (i == 0 ? 1.15f : 1.0f);
         spheres[count][2] = MIN(2.0f, fabsf(rm->gwheel_vel) / 300.0f);
         spheres[count][3] = (i == 0) ? 0.0f : 0.5f;
         colors[count][0]  = rm->sys[rm->system].rgb[0] * 1.1f;
         colors[count][1]  = rm->sys[rm->system].rgb[1] * 1.1f;
         colors[count][2]  = rm->sys[rm->system].rgb[2] * 1.1f;
         count++;
      }
   }

   glViewport(0, 0, width, height);
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_BLEND);
   glUseProgram(rm->prog);
   glBindVertexArray(rm->vao);
   glUniform2f(rm->u_size, (float)width, (float)height);
   glUniform2fv(rm->u_pivot, 1, pivot);
   glUniform2f(rm->u_cam, pivot[0] - ps[0], pivot[1] - ps[1]);
   glUniform1f(rm->u_zoom, zoomv);
   glUniform1f(rm->u_fade, rm->fade);
   glUniform1f(rm->u_radius, radius);
   glUniform1f(rm->u_sphere_r, RAMEN_SPHERE_R * S);
   glUniform1i(rm->u_count, count);
   glUniform1f(rm->u_mon, ramen_smooth(rm->mon));
   glUniform4f(rm->u_mon_rect, mon_rect[0], mon_rect[1], mon_rect[2],
         mon_rect[3]);
   if (count)
   {
      glUniform4fv(rm->u_spheres, count, &spheres[0][0]);
      glUniform3fv(rm->u_colors, count, &colors[0][0]);
      glUniform1fv(rm->u_alpha, count, alphas);
      glUniform1fv(rm->u_art, count, arts);
   }
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D_ARRAY, rm->art_tex);
   glUniform1i(rm->u_art_tex, 0);
   /* The game sampler lives on unit 1 even when unused: two samplers of
    * different types on one unit make the whole draw fail. */
   glUniform1i(rm->u_game, 1);
   /* the in-game menu samples the paused frame straight out of gl2 */
   glUniform1f(rm->u_ig, rm->state == RAMEN_INGAME ? 1.0f : 0.0f);
   glUniform2f(rm->u_ig_open, rm->ig_h, ramen_smooth(rm->ig_v));
   glUniform1f(rm->u_degauss,
         (rm->ig_degauss >= 0.0f && rm->ig_degauss < RAMEN_DEGAUSS_S)
         ? rm->ig_degauss : -1.0f);
   hud_top = ramen_hud_layout(rm, width, height, hud_vx, hud_vy, hud_tx, hud_sc);
   glUniform1f(rm->u_hud, rm->hud);
   if (rm->hud > 0.0f)
   {
      /* the battery green, amber at 20%, red at 10%; the volume blue */
      float pct      = rm->bat_pct < 0 ? 0.0f : rm->bat_pct / 100.0f;
      float vial[2][4] = {
         { hud_vx[0], hud_vy[0], pct, rm->bat_charging ? 1.0f : 0.0f },
         { hud_vx[1], hud_vy[1], rm->vol < 0 ? 0.0f
                              : (float)rm->vol / RAMEN_VOLUME_MAX, 0.0f } };
      float rgb[2][3]  = {
         { 0.35f, 0.85f, 0.45f },
         { 0.35f, 0.65f, 1.00f } };
      if (rm->bat_pct >= 0 && rm->bat_pct <= 10)
      {
         rgb[0][0] = 0.95f; rgb[0][1] = 0.25f; rgb[0][2] = 0.20f;
      }
      else if (rm->bat_pct >= 0 && rm->bat_pct <= 20)
      {
         rgb[0][0] = 1.00f; rgb[0][1] = 0.62f; rgb[0][2] = 0.18f;
      }
      glUniform4fv(rm->u_vial, 2, &vial[0][0]);
      glUniform3fv(rm->u_vial_rgb, 2, &rgb[0][0]);
      glUniform3f(rm->u_vial_dim, RAMEN_VIAL_HW * S, RAMEN_VIAL_HH * S,
            RAMEN_VIAL_CAP * S);
      glUniform1f(rm->u_phosphor, rm->state == RAMEN_INGAME ? 1.0f : 0.0f);
      glUniform1f(rm->u_time, rm->clock);
   }
   if (rm->state == RAMEN_INGAME)
   {
      gl2_t *gl = (gl2_t*)video_driver_get_ptr();
      GLuint tex = 0;
      if (gl)
      {
         unsigned ti = gl->tex_index;
         tex = gl->texture[ti];
         glUniform4f(rm->u_game_rect, (float)gl->vp.x,
               (float)height - (float)(gl->vp.y + (int)gl->vp.height),
               (float)gl->vp.width, (float)gl->vp.height);
         glUniform2f(rm->u_game_uv,
               gl->tex_w ? (float)gl->last_width[ti] / (float)gl->tex_w : 1.0f,
               gl->tex_h ? (float)gl->last_height[ti] / (float)gl->tex_h : 1.0f);
         /* gl2 uploads software frames top row first, but a hw core
          * renders with GL's bottom-left origin and gl2 flips it on draw */
         {
            struct retro_hw_render_callback *hwr = video_driver_get_hw_context();
            glUniform1f(rm->u_game_flip, (hwr
                     && hwr->context_type != RETRO_HW_CONTEXT_NONE
                     && hwr->bottom_left_origin) ? 1.0f : 0.0f);
         }
      }
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, tex);
      glActiveTexture(GL_TEXTURE0);
   }
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
   if (rm->state == RAMEN_INGAME)
   {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, 0);
      glActiveTexture(GL_TEXTURE0);
   }
   glBindVertexArray(0);
   glUseProgram(0);

   if (rm->state == RAMEN_GAME || rm->state == RAMEN_STARTING)
      ramen_art_prefetch(rm);

   /* Text. The font driver binds its own program and blend state. */
   if (rm->font && rm->fade > 0.0f)
   {
      unsigned alpha = (unsigned)(rm->fade * 255.0f);

      if (rm->hud > 0.0f)
      {
         /* beside the vials: the battery in percent, the volume as X
          * (muted), 1-14 or MAX */
         unsigned ha  = (unsigned)(rm->hud * alpha);
         uint32_t hc  = rm->state == RAMEN_INGAME
            ? COLOR_ABGR(130, 205, 160, 230 * ha / 255)
            : COLOR_ABGR(190, 205, 220, ha);
         float lift   = rm->font_size * hud_sc * 0.35f;
         char bat[16], vol[16];

         if (rm->bat_pct < 0)
            strlcpy(bat, "--", sizeof(bat));
         else
            snprintf(bat, sizeof(bat), "%d%%", rm->bat_pct);
         if (rm->vol < 0)
            strlcpy(vol, "--", sizeof(vol));
         else if (rm->vol == 0)
            strlcpy(vol, "X", sizeof(vol));
         else if (rm->vol >= RAMEN_VOLUME_MAX)
            strlcpy(vol, "MAX", sizeof(vol));
         else
            snprintf(vol, sizeof(vol), "%d", rm->vol);
         gfx_display_draw_text(rm->font, bat, hud_tx[0], hud_vy[0] + lift, width, height,
               hc, TEXT_ALIGN_LEFT, hud_sc, false, 0.0f, true);
         gfx_display_draw_text(rm->font, vol, hud_tx[1], hud_vy[1] + lift, width, height,
               hc, TEXT_ALIGN_LEFT, hud_sc, false, 0.0f, true);
      }

      if (rm->state == RAMEN_SYSTEM && rm->label > 0.0f)
      {
         /* The label surfaces through the selected sphere: it rises a few
          * pixels and fades in. (The centre-out reveal and the blur come
          * later, once the text goes through a texture.) */
         int sys     = ramen_selected_system(rm);
         float a     = RAMEN_SELECT_DEG * (float)M_PI / 180.0f;
         float ease  = 1.0f - (1.0f - rm->label) * (1.0f - rm->label);
         float cx    = pivot[0] - radius * cosf(a);
         float cy    = pivot[1] - radius * sinf(a);
         float rise  = RAMEN_SPHERE_R * S * (0.35f - 0.15f * ease);

         gfx_display_draw_text(rm->font, rm->sys[sys].name,
               cx, cy + rise + rm->font_size * 0.35f, width, height,
               COLOR_ABGR(255, 255, 255, (unsigned)(ease * alpha)),
               TEXT_ALIGN_CENTER, 1.0f, false, 0.0f, true);
      }
      if (rm->state == RAMEN_SYSTEM && rm->notice_t > 0.0f)
         gfx_display_draw_text(rm->font, rm->notice, width * 0.06f,
               height * 0.14f, width, height,
               COLOR_ABGR(255, 200, 120, alpha), TEXT_ALIGN_LEFT, 0.45f,
               false, 0.0f, true);
      else if (rm->state == RAMEN_USB)
      {
         float x = width * 0.06f;
         float y = height * 0.14f;
         gfx_display_draw_text(rm->font, "Storage Mode", x, y, width, height,
               COLOR_ABGR(255, 255, 255, alpha), TEXT_ALIGN_LEFT, 0.7f,
               false, 0.0f, true);
         gfx_display_draw_text(rm->font, "Data is shared over USB.", x,
               y + rm->font_size * 0.9f, width, height,
               COLOR_ABGR(190, 205, 220, alpha), TEXT_ALIGN_LEFT, 0.45f,
               false, 0.0f, true);
         gfx_display_draw_text(rm->font, rm->sd_present
               ? "SD card: in, safe to remove." : "SD card: none.", x,
               y + rm->font_size * 1.5f, width, height,
               COLOR_ABGR(190, 205, 220, alpha), TEXT_ALIGN_LEFT, 0.45f,
               false, 0.0f, true);
         gfx_display_draw_text(rm->font, "Eject the drive, or press Start.", x,
               y + rm->font_size * 2.1f, width, height,
               COLOR_ABGR(190, 205, 220, alpha), TEXT_ALIGN_LEFT, 0.45f,
               false, 0.0f, true);
         if (rm->notice_t > 0.0f)
            gfx_display_draw_text(rm->font, rm->notice, x,
                  y + rm->font_size * 3.0f, width, height,
                  COLOR_ABGR(255, 200, 120, alpha), TEXT_ALIGN_LEFT, 0.45f,
                  false, 0.0f, true);
      }
      else if (rm->state == RAMEN_INGAME)
      {
         /* The page, once the raster is open and the degauss has settled:
          * a title, then rows with the label left and the value right,
          * scrolled to keep the cursor in view. */
         if (ramen_ig_page_shown(rm))
         {
            char tbuf[256], label[256], value[256];
            const char *title = ramen_ig_title(rm, tbuf, sizeof(tbuf));
            float sc   = 0.5f;
            float lh   = rm->font_size * sc * 1.5f;
            float x0   = width * 0.10f;
            float x1   = width * 0.90f;
            float y    = height * 0.13f;
            int n      = ramen_ig_count(rm);
            int sel    = rm->ig_sel[rm->ig_page];
            int rows   = (int)((MIN(height * 0.88f, hud_top - lh * 0.6f) - (y + lh * 1.6f)) / lh);
            int first  = 0, r;

            if (rows < 1)
               rows = 1;
            if (n > rows)
               first = MAX(0, MIN(sel - rows / 2, n - rows));
            strlcpy(tbuf, title, sizeof(tbuf));
            if (title != tbuf)
               strlcpy(tbuf, title, sizeof(tbuf));
            ramen_fit_text(rm->font, tbuf, 0.6f, x1 - x0);
            gfx_display_draw_text(rm->font, tbuf, x0, y, width, height,
                  COLOR_ABGR(225, 255, 235, 255), TEXT_ALIGN_LEFT, 0.6f,
                  false, 0.0f, true);
            y += lh * 1.6f;
            for (r = first; r < n && r < first + rows; r++)
            {
               bool cur = (r == sel);
               uint32_t lc = cur ? COLOR_ABGR(240, 255, 245, 255)
                                 : COLOR_ABGR(130, 205, 160, 230);
               uint32_t vc = cur ? COLOR_ABGR(200, 255, 220, 255)
                                 : COLOR_ABGR(110, 180, 140, 220);
               float lw, over;

               ramen_ig_row(rm, r, label, sizeof(label), value, sizeof(value));
               lw = value[0] ? (x1 - x0) * 0.58f : (x1 - x0);
               lw -= rm->font_size * sc;
               if (cur)
                  gfx_display_draw_text(rm->font, ">", x0 - rm->font_size * sc * 0.9f,
                        y, width, height, lc, TEXT_ALIGN_LEFT, sc, false, 0.0f, true);
               over = (float)font_driver_get_message_width(rm->font, label,
                     strlen(label), sc) - lw;
               if (cur && over > 0.0f)
               {
                  /* scroll it inside its column: hold, run to the end,
                   * hold, run back */
                  float speed  = RAMEN_IG_SCROLL_EMS * rm->font_size * sc;
                  float travel = over / speed;
                  float cycle  = 2.0f * (RAMEN_IG_SCROLL_HOLD + travel);
                  float u      = fmodf(rm->ig_scroll_t, cycle);
                  float off;

                  if (u < RAMEN_IG_SCROLL_HOLD)
                     off = 0.0f;
                  else if (u < RAMEN_IG_SCROLL_HOLD + travel)
                     off = (u - RAMEN_IG_SCROLL_HOLD) * speed;
                  else if (u < 2.0f * RAMEN_IG_SCROLL_HOLD + travel)
                     off = over;
                  else
                     off = over - (u - 2.0f * RAMEN_IG_SCROLL_HOLD - travel) * speed;
                  glEnable(GL_SCISSOR_TEST);
                  glScissor((GLint)x0, (GLint)(height - (y + lh * 0.45f)),
                        (GLsizei)lw, (GLsizei)lh);
                  gfx_display_draw_text(rm->font, label, x0 - off, y, width,
                        height, lc, TEXT_ALIGN_LEFT, sc, false, 0.0f, true);
                  glDisable(GL_SCISSOR_TEST);
               }
               else
               {
                  ramen_fit_text(rm->font, label, sc, lw);
                  gfx_display_draw_text(rm->font, label, x0, y, width, height,
                        lc, TEXT_ALIGN_LEFT, sc, false, 0.0f, true);
               }
               if (value[0])
               {
                  ramen_fit_text(rm->font, value, sc, (x1 - x0) * 0.40f);
                  gfx_display_draw_text(rm->font, value, x1, y, width, height,
                        vc, TEXT_ALIGN_RIGHT, sc, false, 0.0f, true);
               }
               y += lh;
            }
            if (n > rows)
            {
               snprintf(label, sizeof(label), "%d / %d", sel + 1, n);
               gfx_display_draw_text(rm->font, label, x1, height * 0.94f,
                     width, height, COLOR_ABGR(110, 180, 140, 200),
                     TEXT_ALIGN_RIGHT, 0.4f, false, 0.0f, true);
            }
         }
      }
      else if ((rm->state == RAMEN_GAME || rm->state == RAMEN_STARTING
               || rm->state == RAMEN_RETURNING) && rm->mon > 0.85f)
      {
         /* The monitor's picture: title, system, play time and last
          * played, laid out in world space and projected like the rest.
          * Text shows once the raster has opened most of the way. */
         const char *path = ramen_game_path(rm, rm->system, rm->game);
         float mon_a   = ramen_smooth((rm->mon - 0.85f) / 0.15f);
         unsigned ta   = (unsigned)(mon_a * alpha);
         float inset   = mon_rect[2] * RAMEN_MON_INSET;
         /* world -> screen */
         float sx      = ps[0] + (mon_rect[0] + inset * 1.6f - pivot[0]) * zoomv;
         float sw      = (mon_rect[2] - inset * 3.2f) * zoomv;
         float sy      = ps[1] + (mon_rect[1] + inset * 1.8f - pivot[1]) * zoomv;
         float lh      = rm->font_size * 0.55f * zoomv;
         char title[256];
         char l1[256], l2[256];
         char line[96];
         float y;
         int lines;

         if (path)
            ramen_game_title(path, title, sizeof(title));
         else
         {
            /* "No games in roms/a, b or c" */
            const ramen_system_t *s = &rm->sys[rm->system];
            int d;

            snprintf(title, sizeof(title), "No games in roms/%s",
                  s->dirs[0]);
            for (d = 1; d < s->ndirs; d++)
            {
               strlcat(title, d == s->ndirs - 1 ? " or " : ", ",
                     sizeof(title));
               strlcat(title, s->dirs[d], sizeof(title));
            }
         }
         /* the title gets up to two lines; the details follow below */
         lines = ramen_wrap_text(rm->font, title, 0.55f * zoomv, sw,
               l1, l2, sizeof(l1));
         gfx_display_draw_text(rm->font, l1,
               sx, sy + lh * 0.8f, width, height,
               COLOR_ABGR(255, 235, 245, 240 * ta / 255),
               TEXT_ALIGN_LEFT, 0.55f * zoomv, false, 0.0f, true);
         if (lines > 1)
            gfx_display_draw_text(rm->font, l2,
                  sx, sy + lh * 1.9f, width, height,
                  COLOR_ABGR(255, 235, 245, 240 * ta / 255),
                  TEXT_ALIGN_LEFT, 0.55f * zoomv, false, 0.0f, true);
         y = sy + lh * (lines > 1 ? 3.4f : 2.3f);
         if (path)
         {
            ramen_refresh_meta(rm);
            snprintf(line, sizeof(line), "%s   %d / %u",
                  rm->sys[rm->system].name, rm->game + 1,
                  ramen_game_count(rm, rm->system));
            gfx_display_draw_text(rm->font, line,
                  sx, y, width, height,
                  COLOR_ABGR(140, 210, 165, 220 * ta / 255),
                  TEXT_ALIGN_LEFT, 0.42f * zoomv, false, 0.0f, true);
            gfx_display_draw_text(rm->font, rm->meta_runtime,
                  sx, y + lh * 1.3f, width, height,
                  COLOR_ABGR(140, 210, 165, 220 * ta / 255),
                  TEXT_ALIGN_LEFT, 0.42f * zoomv, false, 0.0f, true);
            if (rm->meta_last[0])
               gfx_display_draw_text(rm->font, rm->meta_last,
                     sx, y + lh * 2.4f, width, height,
                     COLOR_ABGR(140, 210, 165, 220 * ta / 255),
                     TEXT_ALIGN_LEFT, 0.42f * zoomv, false, 0.0f, true);
         }
      }
   }
   glDisable(GL_BLEND);
}

static int ramen_read_int(const char *path)
{
   char buf[32];

   return ramen_read_line(path, buf, sizeof(buf)) ? atoi(buf) : -1;
}

static void ramen_write_int(const char *path, int v)
{
   FILE *f = fopen(path, "w");

   if (!f)
      return;
   fprintf(f, "%d\n", v);
   fclose(f);
}

static void ramen_saver(ramen_handle_t *rm, bool on)
{
   if (on && !rm->saver)
   {
      int cur = ramen_read_int(RAMEN_BACKLIGHT_DIR "/brightness");
      int max = ramen_read_int(RAMEN_BACKLIGHT_DIR "/max_brightness");

      if (cur <= 0 || max <= 0)
         return;
      rm->saver_brightness = cur;
      rm->saver            = true;
      ramen_write_int(RAMEN_BACKLIGHT_DIR "/brightness",
            MAX(1, MIN(cur, max * RAMEN_SAVER_DIM_PCT / 100)));
   }
   else if (!on && rm->saver)
   {
      /* miroki-inputd's level wins: it may have changed while dimmed */
      int want = ramen_read_int(RAMEN_BACKLIGHT_LEVEL);

      rm->saver = false;
      ramen_write_int(RAMEN_BACKLIGHT_DIR "/brightness",
            want > 0 ? want : rm->saver_brightness);
   }
}

/* RetroArch's menu screensaver. Answering DISABLE at init is also how
 * RetroArch learns the driver supports one. */
static int ramen_environ(enum menu_environ_cb type, void *data, void *userdata)
{
   ramen_handle_t *rm = (ramen_handle_t*)userdata;

   switch (type)
   {
      case MENU_ENVIRON_ENABLE_SCREENSAVER:
         if (rm)
            ramen_saver(rm, true);
         return 0;
      case MENU_ENVIRON_DISABLE_SCREENSAVER:
         if (rm)
            ramen_saver(rm, false);
         return 0;
      default:
         return -1;
   }
}

/* Nothing on screen is moving: the idle frame rate won't show. */
static bool ramen_still(ramen_handle_t *rm)
{
   bool hud_still = rm->hud <= 0.0f || rm->hud >= 1.0f;

   if (rm->fade < 1.0f || rm->launch > 0.0f || rm->usb_req)
      return false;
   switch (rm->state)
   {
      case RAMEN_SYSTEM:
         return rm->settled && !rm->boot_spin && rm->label >= 1.0f
            && rm->jiggle <= 0.0f && rm->notice_t <= 0.0f && hud_still
            && !rm->drag && fabsf(rm->wheel_vel) < 2.0f
            && fabsf(rm->wheel - rm->wheel_target) < 0.1f;
      case RAMEN_USB:
         return rm->notice_t <= 0.0f;
      case RAMEN_GAME:
         return rm->gsettled && rm->mon >= 1.0f
            && rm->gintro >= rm->gintro_len
            && !rm->drag && !rm->gcoast && fabsf(rm->gwheel_vel) < 2.0f
            && fabsf(rm->gwheel - rm->gwheel_target) < 0.1f;
      case RAMEN_INGAME:
         return rm->ig_v >= 1.0f && !rm->ig_closing
            && !(rm->ig_degauss >= 0.0f && rm->ig_degauss < RAMEN_DEGAUSS_S)
            && hud_still;
      default:
         return false;
   }
}

/*
 * Called by the run loop each iteration, outside the video driver's frame.
 * Loading content reinitialises the video driver on the spot, so it must
 * not happen from frame(): gl2 would carry on with a driver it just freed.
 */
static void ramen_render(void *data, unsigned width, unsigned height,
      bool is_idle)
{
   ramen_handle_t *rm = (ramen_handle_t*)data;
   retro_time_t now;

   if (!rm)
      return;

   /* Idle pacing: stretch this pass to RAMEN_IDLE_FRAME_US once the
    * scene is still and input has been quiet for RAMEN_IDLE_AFTER_US, or
    * to RAMEN_SAVER_FRAME_US under the screensaver. */
   now = cpu_features_get_time_usec();
   if (!is_idle && rm->idle_mark)
   {
      retro_time_t target = 0;

      if (rm->saver)
         target = RAMEN_SAVER_FRAME_US;
      else if (ramen_still(rm)
            && now - menu_state_get_ptr()->input_last_time_us > RAMEN_IDLE_AFTER_US)
         target = RAMEN_IDLE_FRAME_US;
      if (now - rm->idle_mark < target)
         retro_sleep((unsigned)((target - (now - rm->idle_mark)) / 1000));
   }
   rm->idle_mark = cpu_features_get_time_usec();

   if (rm->usb_req)
      ramen_usb_run(rm);
   /* Black, and the core takes over from here. */
   if (rm->state == RAMEN_STARTING && rm->fade <= 0.0f && !rm->started)
   {
      rm->started = true;
      ramen_start_game(rm);
   }
   /* The core went away under the in-game menu (Exit Game, or the core
    * quitting on its own). The menu was already up, so RetroArch does
    * not toggle it again: notice here and play the return. */
   if (rm->state == RAMEN_INGAME && retroarch_ctl(RARCH_CTL_IS_DUMMY_CORE, NULL))
   {
      ramen_return_from_game(rm);
      return;
   }
   /* The in-game menu has collapsed: do what was picked. Resuming closes
    * the menu; unloading the core is noticed above on a later pass. */
   if (rm->state == RAMEN_INGAME && rm->ig_closing && rm->ig_h <= 0.0f
         && rm->ig_cmd != IG_CMD_NONE)
   {
      enum ramen_ig_cmd cmd = rm->ig_cmd;
      rm->ig_cmd = IG_CMD_NONE;
      switch (cmd)
      {
         case IG_CMD_SAVE:
            command_event(CMD_EVENT_SAVE_STATE, NULL);
            command_event(CMD_EVENT_RESUME, NULL);
            break;
         case IG_CMD_LOAD:
            command_event(CMD_EVENT_LOAD_STATE, NULL);
            command_event(CMD_EVENT_RESUME, NULL);
            break;
         case IG_CMD_EXIT:
            command_event(CMD_EVENT_UNLOAD_CORE, NULL);
            break;
         case IG_CMD_RESUME:
         default:
            command_event(CMD_EVENT_RESUME, NULL);
            break;
      }
   }
}

static int ramen_entry_action(void *userdata, menu_entry_t *entry,
      size_t i, enum menu_action action)
{
   ramen_handle_t *rm = (ramen_handle_t*)userdata;

   if (!rm)
      return -1;

   /* MENU_ACTION_NOOP arrives every iteration nothing is pressed, and the
    * pointer actions are not ours: none of them count as input. */
   switch (action)
   {
      case MENU_ACTION_LEFT:
      case MENU_ACTION_DOWN:
      case MENU_ACTION_RIGHT:
      case MENU_ACTION_UP:
      case MENU_ACTION_OK:
      case MENU_ACTION_CANCEL:
      case MENU_ACTION_START:
      case MENU_ACTION_SELECT:
      case MENU_ACTION_INFO:
      case MENU_ACTION_SEARCH:
      case MENU_ACTION_SCAN:
         break;
      default:
         return 0;
   }

   if (rm->boot_spin)
      ramen_snap(rm);

   switch (rm->state)
   {
      case RAMEN_SYSTEM:
         switch (action)
         {
            /* Down and LEFT turn the wheel clockwise, Up and RIGHT the
             * other way: matching the rotation to the press felt better
             * than matching the sphere's travel. */
            case MENU_ACTION_DOWN:
            case MENU_ACTION_LEFT:
               ramen_step(rm, -1);
               break;
            case MENU_ACTION_UP:
            case MENU_ACTION_RIGHT:
               ramen_step(rm, 1);
               break;
            case MENU_ACTION_START:
               if (rm->settled && !rm->usb_req)
               {
                  rm->usb_req = 1;
                  ramen_snd_play(SND_CONFIRM);
               }
               break;
            case MENU_ACTION_OK:
               if (!rm->settled)
                  break;
               rm->system        = ramen_selected_system(rm);
               rm->jiggle        = 1.6f;
               ramen_snd_play(SND_CONFIRM);
               rm->seq           = 0.0f;
               rm->state         = RAMEN_LAUNCHING;
               rm->gwheel        = rm->gwheel_target = 0.0f;
               rm->gwheel_vel    = 0.0f;
               rm->gsettled      = true;
               ramen_load_games(rm, rm->system);
               rm->gslot = ramen_game_slot(ramen_game_count(rm, rm->system));
               if ((unsigned)rm->game >= ramen_game_count(rm, rm->system))
                  rm->game = 0;
               ramen_art_prefetch(rm);   /* a head start on the burst */
               /* the intro lasts until the last sphere that can be seen,
                * the one nearest the bottom edge, has settled */
               {
                  int vis = (int)((RAMEN_GAME_END - RAMEN_GAME_DEG)
                                  / rm->gslot) + 1;
                  int n   = (int)ramen_game_count(rm, rm->system);
                  vis = MIN(vis, MIN(n, RAMEN_GAMES_MAX));
                  rm->gintro_len = RAMEN_INTRO_BURST
                     + (vis > 0 ? (vis - 1) : 0) * RAMEN_INTRO_STAGGER;
               }
               break;
            default:
               break;
         }
         break;

      case RAMEN_LAUNCHING:
      case RAMEN_RETURNING:
         /* Let the motion finish; a press mid-way just reverses it. */
         if (action == MENU_ACTION_CANCEL && rm->state == RAMEN_LAUNCHING)
         {
            /* no games are out yet, so nothing to drop */
            rm->state  = RAMEN_RETURNING;
            rm->goutro = RAMEN_OUTRO_S;
         }
         break;

      case RAMEN_GAME:
         if (rm->launch > 0.0f)
            break;
         switch (action)
         {
            /* Down and Right turn the queue clockwise,
             * which is forward through the list. */
            case MENU_ACTION_DOWN:
            case MENU_ACTION_RIGHT:
               if (rm->gintro >= rm->gintro_len)
                  ramen_game_step(rm, 1);
               break;
            case MENU_ACTION_UP:
            case MENU_ACTION_LEFT:
               if (rm->gintro >= rm->gintro_len)
                  ramen_game_step(rm, -1);
               break;
            case MENU_ACTION_OK:
               if (ramen_game_path(rm, rm->system, rm->game))
               {
                  rm->state   = RAMEN_STARTING;
                  rm->started = false;
                  ramen_snd_play(SND_CONFIRM);
               }
               break;
            case MENU_ACTION_CANCEL:
               rm->state  = RAMEN_RETURNING;
               rm->goutro = 0.0f;
               ramen_snd_play(SND_BACK);
               break;
            default:
               break;
         }
         break;

      case RAMEN_STARTING:
         break;

      case RAMEN_USB:
         if ((action == MENU_ACTION_START || action == MENU_ACTION_CANCEL)
               && !rm->usb_req)
            rm->usb_req = 2;
         break;

      case RAMEN_INGAME:
      {
         int n = ramen_ig_count(rm);
         int *sel = &rm->ig_sel[rm->ig_page];
         /* nothing while the raster moves or the picture is degaussing */
         if (rm->ig_closing || rm->ig_v < 1.0f
               || (rm->ig_degauss >= 0.0f && rm->ig_degauss < RAMEN_DEGAUSS_S))
            break;
         if (*sel >= n)
            *sel = n ? n - 1 : 0;
         switch (action)
         {
            case MENU_ACTION_UP:
               if (n) *sel = (*sel + n - 1) % n;
               ramen_snd_play(SND_TICK);
               break;
            case MENU_ACTION_DOWN:
               if (n) *sel = (*sel + 1) % n;
               ramen_snd_play(SND_TICK);
               break;
            case MENU_ACTION_LEFT:
               ramen_ig_adjust(rm, -1);
               ramen_snd_play(SND_TICK);
               break;
            case MENU_ACTION_RIGHT:
               ramen_ig_adjust(rm, 1);
               ramen_snd_play(SND_TICK);
               break;
            case MENU_ACTION_OK:
               ramen_ig_activate(rm);
               break;
            case MENU_ACTION_CANCEL:
               if (rm->ig_page == IG_TOP)
                  ramen_ig_close(rm, IG_CMD_RESUME);
               else
                  ramen_ig_goto(rm, rm->ig_page == IG_REMAPS ? IG_OPTIONS : IG_TOP);
               break;
            default:
               break;
         }
         break;
      }
   }
   /* Everything is handled here; the generic entry list never runs. */
   return 0;
}

/* The menu coming up. After a launch that means one of two things: the
 * game has ended and RetroArch is back to the dummy core, so fade in on
 * the monitor's glass and pull back out to the game view; or the game is
 * still running and the in-game menu was toggled. */
static void ramen_toggle(void *data, bool on)
{
   ramen_handle_t *rm = (ramen_handle_t*)data;
   bool running;

   if (rm && !on)
   {
      /* a game is taking over: nothing to shake, and no finger to lift */
      ramen_imu_set(rm, false);
      rm->drag       = 0;
      rm->gcoast     = false;
      rm->touch_down = false;
   }
   if (!rm || !on || !rm->started)
      return;
   running      = !retroarch_ctl(RARCH_CTL_IS_DUMMY_CORE, NULL);
   rm->started  = running;
   rm->seq      = 1.0f;
   rm->mon      = 1.0f;
   rm->gintro   = 100.0f;
   rm->goutro   = 0.0f;
   rm->snap_cam = true;
   if (running)
   {
      /* the in-game menu, powering on over the paused frame */
      rm->state      = RAMEN_INGAME;
      rm->ig_page    = IG_TOP;
      memset(rm->ig_sel, 0, sizeof(rm->ig_sel));
      rm->ig_v       = 0.0f;
      rm->ig_h       = 1.0f;
      rm->ig_closing = false;
      rm->ig_cmd     = IG_CMD_NONE;
      rm->ig_degauss = -1.0f;
      rm->ig_scroll_page = -1;   /* restarts the label scroll */
      rm->fade       = 1.0f;
      rm->launch     = 0.0f;
      ramen_snd_play(SND_POWER);
   }
   else
      ramen_return_from_game(rm);
}

static void ramen_render_messagebox(void *data, const char *msg)
{
   /* TODO: draw on the monitor. Until then, keep the message visible in
    * the log at least. */
   RARCH_LOG("[ramen] message: %s\n", msg);
}

menu_ctx_driver_t menu_ctx_ramen = {
   NULL,                        /* set_texture */
   ramen_render_messagebox,
   ramen_render,
   ramen_frame,
   ramen_init,
   ramen_free,
   ramen_context_reset,
   ramen_context_destroy,
   NULL,                        /* populate_entries */
   ramen_toggle,
   NULL,                        /* navigation_clear */
   NULL,                        /* navigation_decrement */
   NULL,                        /* navigation_increment */
   NULL,                        /* navigation_set */
   NULL,                        /* navigation_set_last */
   NULL,                        /* navigation_descend_alphabet */
   NULL,                        /* navigation_ascend_alphabet */
   NULL,                        /* lists_init */
   NULL,                        /* list_insert */
   NULL,                        /* list_prepend */
   NULL,                        /* list_free */
   NULL,                        /* list_clear */
   NULL,                        /* list_cache */
   NULL,                        /* list_push */
   NULL,                        /* list_get_selection */
   NULL,                        /* list_get_size */
   NULL,                        /* list_get_entry */
   NULL,                        /* list_set_selection */
   NULL,                        /* bind_init */
   NULL,                        /* load_image */
   "ramen",
   ramen_environ,               /* environ_cb */
   NULL,                        /* update_thumbnail_path */
   NULL,                        /* update_thumbnail_image */
   NULL,                        /* refresh_thumbnail_image */
   NULL,                        /* set_thumbnail_content */
   NULL,                        /* osk_ptr_at_pos */
   NULL,                        /* update_savestate_thumbnail_path */
   NULL,                        /* update_savestate_thumbnail_image */
   NULL,                        /* pointer_down */
   NULL,                        /* pointer_up */
   ramen_entry_action
};
