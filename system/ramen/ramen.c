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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
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
#include "../../tasks/task_content.h"
#include "../../verbosity.h"
#include "../menu_driver.h"

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
/* Once the sphere has parked, the games come out of it: how long, and
 * from how far inside. The offset exceeds the visible arc so that even
 * the farthest trailing sphere starts inside and the list fills from the
 * burst alone. */
#define RAMEN_INTRO_S     0.5f
#define RAMEN_INTRO_DEG   100.0f

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
   RAMEN_RETURNING,      /* sequence 3 backwards, seq runs 1 -> 0 */
   RAMEN_STARTING        /* fading out, then the core loads */
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
   float gintro;         /* games surfacing out of the parked sphere, 0..1 */
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
   "uniform float u_cam;\n"
   "uniform float u_zoom;\n"
   "uniform float u_fade;\n"
   "uniform float u_radius;\n"
   "uniform float u_sphere_r;\n"
   "uniform int u_count;\n"
   "uniform vec4 u_spheres[16];\n"
   "uniform vec3 u_colors[16];\n"
   "uniform float u_alpha[16];\n"
   "\n"
   "void main() {\n"
   "  float S = min(u_size.x, u_size.y);\n"
   "  vec2 pivot_screen = u_pivot - vec2(u_cam, 0.0);\n"
   "  vec2 w = u_pivot + (v_px - pivot_screen) / u_zoom;\n"
   "  vec2 rel = w - u_pivot;\n"
   "  float dist = length(rel);\n"
   "  vec3 col = vec3(0.035, 0.040, 0.055);\n"
   "  col += vec3(0.02) * (1.0 - clamp(dist / S, 0.0, 1.0));\n"
   "\n"
   /* glass tube: a translucent band with an inner specular stripe */
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
   /* specular highlight near the top */
   "    vec2 hl = (q / R) - vec2(-0.30, -0.42);\n"
   "    float sp = exp(-dot(hl, hl) * 14.0);\n"
   "    base += vec3(1.0) * sp * 0.9;\n"
   "    base *= (1.0 - dim * 0.45);\n"
   "    float alpha = edge * (0.82 + 0.18 * z) * (1.0 - min(stretch, 1.5) * 0.4) * u_alpha[i];\n"
   "    col = mix(col, base, alpha);\n"
   "  }\n"
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
   rm->prog     = 0;
   rm->vao      = 0;
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
   /* TODO: tick sound */
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

      rm->wheel_vel *= expf(-RAMEN_BOOT_FRICTION * dt);
      rm->wheel     += rm->wheel_vel * dt;
      d              = rm->wheel_target - rm->wheel;
      /* TODO: a tick each time a sphere crosses a slot boundary */
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
      rm->seq -= dt / RAMEN_SEQ_TOTAL;
      if (rm->seq <= 0.0f)
      {
         rm->seq   = 0.0f;
         rm->state = RAMEN_SYSTEM;
         rm->settled = false;  /* lets the label resurface */
      }
   }

   if (rm->state == RAMEN_GAME)
      rm->gintro = MIN(1.0f, rm->gintro + dt / RAMEN_INTRO_S);

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

   if (rm->state == RAMEN_STARTING)
      rm->fade = MAX(0.0f, rm->fade - dt / RAMEN_FADE_S);
   else
      rm->fade = MIN(1.0f, rm->fade + dt / RAMEN_FADE_S);
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

/* ---- launching a game ------------------------------------------------ */

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
   int count            = 0;
   int selected_slot;
   float clear, launch_deg, cam_target, dt;
   bool in_sequence;
   int k;

   if (!rm || !rm->gl_ready)
      return;

   ramen_update(rm, cpu_features_get_time_usec());
   dt            = rm->dt;
   selected_slot = ramen_slot_at(rm->wheel_target);
   in_sequence   = rm->state != RAMEN_SYSTEM;
   clear         = in_sequence ? ramen_clearout(rm->seq) : 0.0f;
   launch_deg    = ramen_launch_angle(rm->seq);

   /* The camera chases the sphere: its target follows the sphere's run
    * through the tube, and it zooms out as it arrives. */
   cam_target = S * ramen_smooth((launch_deg - 60.0f) / (RAMEN_PARK_DEG - 60.0f));
   ramen_spring(&rm->cam, &rm->cam_vel, cam_target, RAMEN_CAM_HZ,
         RAMEN_CAM_ZETA, dt);
   rm->zoom = 1.0f - (1.0f - RAMEN_GAME_ZOOM)
            * ramen_smooth((launch_deg - 100.0f) / (RAMEN_PARK_DEG - 100.0f));

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
      count++;
   }

   /* The games, once the sphere has parked: a ring of n identical spheres
    * drawn relative to the selection, which is bookkeeping on the side.
    * Relative index i sits at RAMEN_GAME_DEG - i * slot, so the selected
    * sphere is on the diagonal and the rest trail clockwise toward the
    * bottom edge, the whole list coming out of the sphere together in the
    * intro. A step turns the ring: one sphere enters from the system sphere
    * (i = 1) while the last one (i = -n) leaves past the edge, and those
    * two only exist while the ring is in motion. */
   if (rm->state == RAMEN_GAME || rm->state == RAMEN_STARTING)
   {
      int n        = (int)ramen_game_count(rm, rm->system);
      int last     = MIN(n, RAMEN_GAMES_MAX);
      float offset = rm->gwheel - rm->gwheel_target;
      float moving = MIN(1.0f, fabsf(offset) / rm->gslot);
      float emerge = RAMEN_PARK_DEG + RAMEN_GAME_EMERGE;
      int i;

      for (i = -last; i <= 1 && count < RAMEN_MAX_SPHERES; i++)
      {
         float deg = RAMEN_GAME_DEG + offset - i * rm->gslot
                   - RAMEN_INTRO_DEG * (1.0f - ramen_burst(rm->gintro));
         float a;

         if (n <= 0)
            continue;
         if (deg < emerge || deg > RAMEN_GAME_END)
            continue;
         /* surfacing out of the sphere, sinking past the edge */
         a = ramen_smooth((deg - emerge) / RAMEN_GAME_FADE)
           * ramen_smooth((RAMEN_GAME_END - deg) / RAMEN_GAME_FADE);
         if (i == 1 || i == -last)
            a *= moving;
         if (a <= 0.0f)
            continue;
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
   glUniform1f(rm->u_cam, rm->cam);
   glUniform1f(rm->u_zoom, rm->zoom);
   glUniform1f(rm->u_fade, rm->fade);
   glUniform1f(rm->u_radius, radius);
   glUniform1f(rm->u_sphere_r, RAMEN_SPHERE_R * S);
   glUniform1i(rm->u_count, count);
   if (count)
   {
      glUniform4fv(rm->u_spheres, count, &spheres[0][0]);
      glUniform3fv(rm->u_colors, count, &colors[0][0]);
      glUniform1fv(rm->u_alpha, count, alphas);
   }
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glBindVertexArray(0);
   glUseProgram(0);

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
      else if (rm->state == RAMEN_GAME || rm->state == RAMEN_STARTING)
      {
         /* Until the monitor exists, the title sits in the space it will
          * occupy, top right. */
         const char *path = ramen_game_path(rm, rm->system, rm->game);
         char title[256];
         char count_s[32];

         if (path)
            ramen_game_title(path, title, sizeof(title));
         else
            snprintf(title, sizeof(title), "No games in %s/%s",
                  RAMEN_ROM_DIR, ramen_systems[rm->system].dir);
         gfx_display_draw_text(rm->font, title,
               width * 0.96f, height * 0.12f, width, height,
               COLOR_ABGR(255, 255, 255, alpha),
               TEXT_ALIGN_RIGHT, 0.8f, false, 0.0f, true);
         if (path)
         {
            snprintf(count_s, sizeof(count_s), "%d / %u", rm->game + 1,
                  ramen_game_count(rm, rm->system));
            gfx_display_draw_text(rm->font, count_s,
                  width * 0.96f, height * 0.12f + rm->font_size,
                  width, height,
                  COLOR_ABGR(180, 200, 220, alpha),
                  TEXT_ALIGN_RIGHT, 0.6f, false, 0.0f, true);
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
   /* Black, and the core takes over from here. */
   if (rm->state == RAMEN_STARTING && rm->fade <= 0.0f && !rm->started)
   {
      rm->started = true;
      ramen_start_game(rm);
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
            /* Down and Right turn the wheel clockwise, Up and Left the
             * other way: matching the rotation to the press felt better
             * than matching the sphere's travel. */
            case MENU_ACTION_LEFT:
            case MENU_ACTION_UP:
               ramen_step(rm, -1);
               break;
            case MENU_ACTION_RIGHT:
            case MENU_ACTION_DOWN:
               ramen_step(rm, 1);
               break;
            case MENU_ACTION_OK:
               if (!rm->settled)
                  break;
               rm->system        = ramen_selected_system(rm);
               rm->jiggle        = 1.0f;
               rm->seq           = 0.0f;
               rm->state         = RAMEN_LAUNCHING;
               rm->gwheel        = rm->gwheel_target = 0.0f;
               rm->gwheel_vel    = 0.0f;
               rm->gsettled      = true;
               ramen_load_games(rm, rm->system);
               rm->gslot = ramen_game_slot(ramen_game_count(rm, rm->system));
               if ((unsigned)rm->game >= ramen_game_count(rm, rm->system))
                  rm->game = 0;
               break;
            default:
               break;
         }
         break;

      case RAMEN_LAUNCHING:
      case RAMEN_RETURNING:
         /* Let the motion finish; a press mid-way just reverses it. */
         if (action == MENU_ACTION_CANCEL && rm->state == RAMEN_LAUNCHING)
            rm->state = RAMEN_RETURNING;
         break;

      case RAMEN_GAME:
         switch (action)
         {
            /* Same convention: Down and Right turn the queue clockwise,
             * which is forward through the list. */
            case MENU_ACTION_DOWN:
            case MENU_ACTION_RIGHT:
               if (rm->gintro >= 1.0f)
                  ramen_game_step(rm, 1);
               break;
            case MENU_ACTION_UP:
            case MENU_ACTION_LEFT:
               if (rm->gintro >= 1.0f)
                  ramen_game_step(rm, -1);
               break;
            case MENU_ACTION_OK:
               if (ramen_game_path(rm, rm->system, rm->game))
               {
                  rm->state   = RAMEN_STARTING;
                  rm->started = false;
               }
               break;
            case MENU_ACTION_CANCEL:
               rm->state = RAMEN_RETURNING;
               break;
            default:
               break;
         }
         break;

      case RAMEN_STARTING:
         break;
   }
   /* Everything is handled here; the generic entry list never runs. */
   return 0;
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
   NULL,                        /* toggle */
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
