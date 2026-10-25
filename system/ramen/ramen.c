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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <compat/strl.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <formats/image.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <retro_miscellaneous.h>
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
#include "../../input/input_driver.h"
#include "../../input/input_remapping.h"

#define RAMEN_FONT_PATH   "/usr/share/miroki/fonts/Audiowide-Regular.ttf"
#define RAMEN_STATE_PATH  "/data/.config/ramen"
#define RAMEN_ROM_DIR     "/data/roms"
#define RAMEN_CORE_DIR    "/usr/lib/libretro"

/* Carousel: 9 slots at 40 degrees holding the 3 systems three times over.
 * The selected slot sits at 45 degrees (the screen diagonal), its neighbours
 * at 5 and 85, half clipped by the bottom and right edges. */
#define RAMEN_SLOTS       9
#define RAMEN_SLOT_DEG    40.0f
#define RAMEN_SELECT_DEG  45.0f
#define RAMEN_SYSTEMS     3

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

/* Box art, shown inside the game spheres: /data/roms/<system>/boxart/
 * <game>.png or .jpg, centre-cropped square and resampled into one layer
 * of a texture array, so the shader can pick a sphere's art by index. The
 * cache holds the spheres around the selection, replacing the least
 * recently used. */
#define RAMEN_ART_DIR     "boxart"
#define RAMEN_ART_SIZE    256
#define RAMEN_ART_LAYERS  12

/* Tube radius and sphere radius as a fraction of the screen side. */
#define RAMEN_TUBE_R      0.65f
#define RAMEN_SPHERE_R    0.16f

/* Detent spring: natural frequency and damping ratio (design: 0.6-0.8). */
#define RAMEN_SPRING_HZ   2.5f
#define RAMEN_SPRING_ZETA 0.3f

/* Boot spin: exponential friction, wheel starts this far back. Under
 * friction alone it would coast to RAMEN_BOOT_OVERRUN degrees past the
 * target; the spring takes over before that, at the point where it brakes
 * rather than pulls, so the wheel never speeds up into the detent. */
#define RAMEN_BOOT_TURNS  3.0f
#define RAMEN_BOOT_FRICTION 3.5f
#define RAMEN_BOOT_OVERRUN  45.0f

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
#define RAMEN_LAUNCH_S    0.6f
#define RAMEN_LAUNCH_ZOOM 2.9f
#define RAMEN_LAUNCH_FADE 0.5f    /* the dive's progress when the fade starts */
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
   SND_CONFIRM,
   SND_BACK,
   SND_THUNK,
   SND_POWER,
   SND_OFF,
   SND_END
};

/* USB storage from the carousel: Start hands /data to the host, a host
 * eject (or Start again) takes it back; /data is polled this often. */
#define RAMEN_USB_POLL_S  0.5f

/* Pivot corner in screen space (origin top-left, y down): the design is
 * written for the open orientation, pivot at the bottom-right. Switch these
 * if the panel comes up with the hinge elsewhere. */
#define RAMEN_PIVOT_X     1.0f
#define RAMEN_PIVOT_Y     1.0f

static const struct
{
   const char *name;
   const char *dir;      /* under RAMEN_ROM_DIR */
   const char *core;     /* under RAMEN_CORE_DIR */
   const char *exts;     /* dir_list_new() filter */
   float rgb[3];
} ramen_systems[RAMEN_SYSTEMS] = {
   { "N64",    "n64",    "mupen64plus_pow2_libretro.so",
     "n64|v64|z64|bin|u1|ndd",
     { 0.22f, 0.60f, 0.95f } },
   { "PS1",    "psx",    "pcsx_rearmed_libretro.so",
     "cue|chd|pbp|m3u|img|iso|bin|mdf|toc|cbn|exe",
     { 0.55f, 0.55f, 0.60f } },
   { "Saturn", "saturn", "yabadabadoo_libretro.so",
     "cue|chd|iso|mds|ccd|m3u|zip",
     { 0.95f, 0.45f, 0.20f } },
};

enum ramen_state
{
   RAMEN_SYSTEM = 0,     /* the carousel */
   RAMEN_LAUNCHING,      /* sequence 3, seq runs 0 -> 1 */
   RAMEN_GAME,           /* the game view */
   RAMEN_RETURNING,      /* games fall out, then seq runs 1 -> 0 */
   RAMEN_STARTING,       /* diving into the glass, then the core loads */
   RAMEN_INGAME,         /* the in-game menu over the paused frame */
   RAMEN_USB             /* /data exported to the host */
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

   /* Boot spin ticks, USB storage, and a notice line in the carousel. */
   int   boot_slot;
   int   usb_req;        /* 1 = export, 2 = take back */
   float usb_poll;
   char  notice[128];
   float notice_t;

   /* The game view: one list per system, loaded on first entry; the
    * game wheel is the capsule queue's rotation, same spring as the big
    * one. */
   struct string_list *games[RAMEN_SYSTEMS];
   bool games_loaded[RAMEN_SYSTEMS];
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
    * found, and when it was last drawn. */
   GLuint art_tex;
   struct
   {
      int sys, game;
      bool present;
      unsigned stamp;
   } art[RAMEN_ART_LAYERS];
   unsigned art_stamp;

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
   GLint u_ig;
   GLint u_ig_open;
   GLint u_degauss;

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
   "uniform float u_ig;\n"
   "uniform vec2 u_ig_open;\n"
   "uniform float u_degauss;\n"
   "\n"
   /* signed distance to a rounded rectangle: half-size h, corner radius r */
   "float rrect(vec2 p, vec2 h, float r) {\n"
   "  vec2 q = abs(p) - h + r;\n"
   "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
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
   "      g = texture(u_game, guv * u_game_uv).rgb;\n"
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
   rm->u_ig       = glGetUniformLocation(rm->prog, "u_ig");
   rm->u_ig_open  = glGetUniformLocation(rm->prog, "u_ig_open");
   rm->u_degauss  = glGetUniformLocation(rm->prog, "u_degauss");

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
   return n;
}

static void ramen_snd_load(void)
{
   static const char *names[] = { "tick", "confirm", "back", "thunk", "power", "off" };
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
   if (audio_driver_mixer_get_stream_state(which) == AUDIO_STREAM_STATE_NONE)
      ramen_snd_load();
   audio_driver_mixer_play_stream(which);
}

/* ---- USB storage ------------------------------------------------------- */

static bool ramen_data_mounted(void)
{
   FILE *f = fopen("/proc/mounts", "r");
   char line[512];
   bool found = false;

   if (!f)
      return false;
   while (fgets(line, sizeof(line), f))
      if (strstr(line, " /data "))
      {
         found = true;
         break;
      }
   fclose(f);
   return found;
}

static void ramen_notice(ramen_handle_t *rm, const char *msg, float secs)
{
   strlcpy(rm->notice, msg, sizeof(rm->notice));
   rm->notice_t = secs;
}

/* /data may have changed under us: forget what was read from it. */
static void ramen_forget_data(ramen_handle_t *rm)
{
   int i;

   for (i = 0; i < RAMEN_SYSTEMS; i++)
   {
      if (rm->games[i])
         string_list_free(rm->games[i]);
      rm->games[i]        = NULL;
      rm->games_loaded[i] = false;
   }
   rm->meta_sys = rm->meta_game = -1;
   for (i = 0; i < RAMEN_ART_LAYERS; i++)
      rm->art[i].sys = -1;
   rm->game = 0;
}

/* Runs the gadget script: from render(), since it blocks for a moment. */
static void ramen_usb_run(ramen_handle_t *rm)
{
   int req = rm->usb_req;
   int r;

   rm->usb_req = 0;
   if (req == 1)
   {
      r = system("usb-gadget storage on");
      if (r == 0)
      {
         rm->state    = RAMEN_USB;
         rm->usb_poll = 0.0f;
         rm->notice[0] = '\0';
      }
      else
         ramen_notice(rm, "Could not share the drive: /data is busy", 4.0f);
   }
   else if (req == 2)
   {
      r = system("usb-gadget storage off");
      if (r == 0 || ramen_data_mounted())
      {
         rm->state = RAMEN_SYSTEM;
         ramen_forget_data(rm);
         ramen_snd_play(SND_BACK);
      }
      else
         ramen_notice(rm, "The host still holds the drive: eject it there first", 4.0f);
   }
}

/* ---- persistence ----------------------------------------------------- */

static int ramen_load_last_system(void)
{
   FILE *f = fopen(RAMEN_STATE_PATH, "r");
   int sys = 0;

   if (!f)
      return 0;
   if (fscanf(f, "%d", &sys) != 1 || sys < 0 || sys >= RAMEN_SYSTEMS)
      sys = 0;
   fclose(f);
   return sys;
}

static void ramen_save_last_system(int sys)
{
   FILE *f = fopen(RAMEN_STATE_PATH, "w");
   if (!f)
      return;
   fprintf(f, "%d\n", sys);
   fclose(f);
}

/* ---- games ----------------------------------------------------------- */

static void ramen_load_games(ramen_handle_t *rm, int sys)
{
   char dir[PATH_MAX_LENGTH];

   if (rm->games_loaded[sys])
      return;
   rm->games_loaded[sys] = true;

   fill_pathname_join_special(dir, RAMEN_ROM_DIR, ramen_systems[sys].dir,
         sizeof(dir));
   rm->games[sys] = dir_list_new(dir, ramen_systems[sys].exts,
         false, false, false, false);
   if (rm->games[sys])
      dir_list_sort(rm->games[sys], false);
   RARCH_LOG("[ramen] %s: %u games in %s\n", ramen_systems[sys].name,
         rm->games[sys] ? (unsigned)rm->games[sys]->size : 0, dir);
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

/* Loads a game's art into the least recently used layer. A game with no
 * art file takes a layer too, marked absent, so it is not retried. */
static void ramen_art_load(ramen_handle_t *rm, int sys, int game)
{
   static const char *exts[] = { "png", "jpg", "jpeg" };
   const char *rom = ramen_game_path(rm, sys, game);
   struct texture_image img;
   char dir[PATH_MAX_LENGTH];
   char base[256];
   char name[300];
   char path[PATH_MAX_LENGTH];
   int k, lru = 0;
   size_t e;

   if (!rom)
      return;
   for (k = 1; k < RAMEN_ART_LAYERS; k++)
      if (rm->art[k].stamp < rm->art[lru].stamp)
         lru = k;
   rm->art[lru].sys     = sys;
   rm->art[lru].game    = game;
   rm->art[lru].present = false;
   rm->art[lru].stamp   = ++rm->art_stamp;

   fill_pathname_join_special(path, RAMEN_ROM_DIR, ramen_systems[sys].dir,
         sizeof(path));
   fill_pathname_join_special(dir, path, RAMEN_ART_DIR, sizeof(dir));
   fill_pathname_base(base, rom, sizeof(base));
   path_remove_extension(base);
   memset(&img, 0, sizeof(img));
   for (e = 0; e < ARRAY_SIZE(exts); e++)
   {
      snprintf(name, sizeof(name), "%s.%s", base, exts[e]);
      fill_pathname_join_special(path, dir, name, sizeof(path));
      if (!path_is_valid(path))
         continue;
      if (image_texture_load(&img, path) && img.pixels
            && img.width && img.height)
         break;
      image_texture_free(&img);
      memset(&img, 0, sizeof(img));
   }
   if (!img.pixels)
      return;

   {
      uint8_t *buf = (uint8_t*)malloc(RAMEN_ART_SIZE * RAMEN_ART_SIZE * 4);
      if (buf)
      {
         ramen_art_resample(&img, buf);
         glBindTexture(GL_TEXTURE_2D_ARRAY, rm->art_tex);
         glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, lru,
               RAMEN_ART_SIZE, RAMEN_ART_SIZE, 1, GL_RGBA, GL_UNSIGNED_BYTE,
               buf);
         glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
         free(buf);
         rm->art[lru].present = true;
      }
   }
   image_texture_free(&img);
}

/* The play time and last-played date of the selected game, out of
 * RetroArch's runtime log, cached until the selection changes. */
static void ramen_refresh_meta(ramen_handle_t *rm)
{
   settings_t *settings = config_get_ptr();
   const char *rom      = ramen_game_path(rm, rm->system, rm->game);
   runtime_log_t *log   = NULL;
   char core[PATH_MAX_LENGTH];

   if (rm->meta_sys == rm->system && rm->meta_game == rm->game)
      return;
   rm->meta_sys  = rm->system;
   rm->meta_game = rm->game;
   rm->meta_runtime[0] = rm->meta_last[0] = '\0';
   if (!rom || !settings)
      return;
   fill_pathname_join_special(core, RAMEN_CORE_DIR,
         ramen_systems[rm->system].core, sizeof(core));
   log = runtime_log_init(rom, core,
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

/* The slot nearest the selected position for a given wheel angle. */
static int ramen_slot_at(float wheel)
{
   int s = (int)lroundf((RAMEN_SELECT_DEG - wheel) / RAMEN_SLOT_DEG);
   return ((s % RAMEN_SLOTS) + RAMEN_SLOTS) % RAMEN_SLOTS;
}

static int ramen_selected_system(ramen_handle_t *rm)
{
   return ramen_slot_at(rm->wheel_target) % RAMEN_SYSTEMS;
}

static void ramen_begin_boot_spin(ramen_handle_t *rm, int system)
{
   /* The slot holding the last-played system nearest the selected position
    * is as good as any, since the carousel repeats. */
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
   rm->game = ((rm->game + dir) % n + n) % n;
   rm->gwheel_target += dir * rm->gslot;
   rm->gsettled       = false;
   ramen_snd_play(SND_TICK);
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

   if (rm->boot_spin)
   {
      float w   = 2.0f * (float)M_PI * RAMEN_SPRING_HZ;
      float d;

      int slot;

      rm->wheel_vel *= expf(-RAMEN_BOOT_FRICTION * dt);
      rm->wheel     += rm->wheel_vel * dt;
      d              = rm->wheel_target - rm->wheel;
      /* a tick each time a sphere crosses a slot boundary */
      slot = (int)floorf((rm->wheel - rm->wheel_target) / RAMEN_SLOT_DEG);
      if (slot != rm->boot_slot)
      {
         rm->boot_slot = slot;
         ramen_snd_play(SND_TICK);
      }
      /* Hand over once the spring's damping would outweigh its pull:
       * w^2 d <= 2 zeta w v. The wheel then only decelerates. */
      if (d <= 2.0f * RAMEN_SPRING_ZETA * rm->wheel_vel / w
            || rm->wheel_vel < 20.0f)
         rm->boot_spin = false;
   }
   else
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

   /* The game queue's own detent spring. */
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

   /* Exported: once the host ejects, the gadget script remounts /data. */
   if (rm->state == RAMEN_USB)
   {
      rm->usb_poll += dt;
      if (rm->usb_poll >= RAMEN_USB_POLL_S)
      {
         rm->usb_poll = 0.0f;
         if (ramen_data_mounted())
         {
            rm->state = RAMEN_SYSTEM;
            ramen_forget_data(rm);
            ramen_snd_play(SND_BACK);
         }
      }
   }

   if (rm->state == RAMEN_INGAME)
   {
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
         return RAMEN_IG_BINDS;
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

static void ramen_ig_save_remaps(ramen_handle_t *rm)
{
   settings_t *settings = config_get_ptr();
   const char *core     = runloop_state_get_ptr()->system.info.library_name;
   char dir[PATH_MAX_LENGTH];
   char path[PATH_MAX_LENGTH];

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
         unsigned cur  = ids[sel];
         bool has_descs = ramen_ig_core_has_descs();
         int pos = RAMEN_IG_BINDS, i, tries;
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
         if (settings && row < RAMEN_IG_BINDS)
         {
            strlcpy(label, input_config_bind_map_get_desc((unsigned)row), llen);
            strlcpy(value, ramen_ig_remap_name(
                     settings->uints.input_remap_ids[0][row]), vlen);
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
   char core[PATH_MAX_LENGTH];
   const char *rom = ramen_game_path(rm, rm->system, rm->game);

   if (!rom)
      return;
   memset(&info, 0, sizeof(info));
   fill_pathname_join_special(core, RAMEN_CORE_DIR,
         ramen_systems[rm->system].core, sizeof(core));
   RARCH_LOG("[ramen] launching %s with %s\n", rom, core);
   ramen_save_last_system(rm->system);
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
   ramen_begin_boot_spin(rm, ramen_load_last_system());
   return menu;
}

/* RetroArch frees the handle itself after this; only its contents go here. */
static void ramen_free(void *data)
{
   ramen_handle_t *rm = (ramen_handle_t*)data;
   int i;

   if (!rm)
      return;
   if (rm->font)
      font_driver_free(rm->font);
   rm->font = NULL;
   for (i = 0; i < RAMEN_SYSTEMS; i++)
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
   int want_sys         = -1;
   int want_game        = -1;
   int selected_slot;
   float clear, launch_deg, cam_target, dt;
   float mon_rect[4];
   float ps[2];          /* the pivot on screen */
   float zoomv;          /* the zoom in effect, dive included */
   bool in_sequence;
   int k;

   if (!rm || !rm->gl_ready)
      return;

   ramen_update(rm, cpu_features_get_time_usec());
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

   /* The system spheres. In a sequence the selected one is driven by the
    * timeline and the neighbours retract past their edges. */
   for (k = 0; k < RAMEN_SLOTS && count < RAMEN_MAX_SPHERES; k++)
   {
      float deg = fmodf(rm->wheel + k * RAMEN_SLOT_DEG, 360.0f);
      float stretch, dim, scale;
      bool selected = (k == selected_slot);

      if (deg < 0.0f)
         deg += 360.0f;
      if (deg > 90.0f + 20.0f && deg < 360.0f - 20.0f)
         continue;

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
      memcpy(colors[count], ramen_systems[k % RAMEN_SYSTEMS].rgb,
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
            /* the game this sphere is, and its art if cached; one missing
             * one is decoded per frame, after the draw, so the hitch is
             * spread out rather than taken all at once */
            int idx   = ((rm->game + i) % n + n) % n;
            int layer = ramen_art_find(rm, rm->system, idx);
            arts[count] = (layer >= 0 && rm->art[layer].present)
                        ? (float)layer : -1.0f;
            if (layer < 0 && want_game < 0)
            {
               want_sys  = rm->system;
               want_game = idx;
            }
         }
         alphas[count]     = a;
         spheres[count][0] = deg * (float)M_PI / 180.0f;
         spheres[count][1] = RAMEN_GAME_SCALE * (i == 0 ? 1.15f : 1.0f);
         spheres[count][2] = MIN(2.0f, fabsf(rm->gwheel_vel) / 300.0f);
         spheres[count][3] = (i == 0) ? 0.0f : 0.5f;
         colors[count][0]  = ramen_systems[rm->system].rgb[0] * 1.1f;
         colors[count][1]  = ramen_systems[rm->system].rgb[1] * 1.1f;
         colors[count][2]  = ramen_systems[rm->system].rgb[2] * 1.1f;
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

   if (want_game >= 0)
      ramen_art_load(rm, want_sys, want_game);

   /* Text. The font driver binds its own program and blend state. */
   if (rm->font && rm->fade > 0.0f)
   {
      unsigned alpha = (unsigned)(rm->fade * 255.0f);

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

         gfx_display_draw_text(rm->font, ramen_systems[sys].name,
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
         gfx_display_draw_text(rm->font, "USB drive", x, y, width, height,
               COLOR_ABGR(255, 255, 255, alpha), TEXT_ALIGN_LEFT, 0.7f,
               false, 0.0f, true);
         gfx_display_draw_text(rm->font, "Shared with the host.", x,
               y + rm->font_size * 0.9f, width, height,
               COLOR_ABGR(190, 205, 220, alpha), TEXT_ALIGN_LEFT, 0.45f,
               false, 0.0f, true);
         gfx_display_draw_text(rm->font, "Eject it there, or press Start.", x,
               y + rm->font_size * 1.5f, width, height,
               COLOR_ABGR(190, 205, 220, alpha), TEXT_ALIGN_LEFT, 0.45f,
               false, 0.0f, true);
         if (rm->notice_t > 0.0f)
            gfx_display_draw_text(rm->font, rm->notice, x,
                  y + rm->font_size * 2.4f, width, height,
                  COLOR_ABGR(255, 200, 120, alpha), TEXT_ALIGN_LEFT, 0.45f,
                  false, 0.0f, true);
      }
      else if (rm->state == RAMEN_INGAME)
      {
         /* The page, once the raster is open and the degauss has settled:
          * a title, then rows with the label left and the value right,
          * scrolled to keep the cursor in view. */
         bool show = rm->ig_v >= 0.97f && !rm->ig_closing
            && !(rm->ig_degauss >= 0.0f && rm->ig_degauss < RAMEN_DEGAUSS_SHOW);
         if (show)
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
            int rows   = (int)((height * 0.88f - (y + lh * 1.6f)) / lh);
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
               float lw;

               ramen_ig_row(rm, r, label, sizeof(label), value, sizeof(value));
               lw = value[0] ? (x1 - x0) * 0.58f : (x1 - x0);
               ramen_fit_text(rm->font, label, sc, lw - rm->font_size * sc);
               if (cur)
                  gfx_display_draw_text(rm->font, ">", x0 - rm->font_size * sc * 0.9f,
                        y, width, height, lc, TEXT_ALIGN_LEFT, sc, false, 0.0f, true);
               gfx_display_draw_text(rm->font, label, x0, y, width, height,
                     lc, TEXT_ALIGN_LEFT, sc, false, 0.0f, true);
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
            snprintf(title, sizeof(title), "No games in %s/%s",
                  RAMEN_ROM_DIR, ramen_systems[rm->system].dir);
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
                  ramen_systems[rm->system].name, rm->game + 1,
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

/*
 * Called by the run loop each iteration, outside the video driver's frame.
 * Loading content reinitialises the video driver on the spot, so it must
 * not happen from frame(): gl2 would carry on with a driver it just freed.
 */
static void ramen_render(void *data, unsigned width, unsigned height,
      bool is_idle)
{
   ramen_handle_t *rm = (ramen_handle_t*)data;

   if (!rm)
      return;
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
               rm->jiggle        = 1.0f;
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
   NULL,                        /* environ_cb */
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
