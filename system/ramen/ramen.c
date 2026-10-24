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
 * Skeleton status: tube, three systems on a nine-slot carousel, the boot spin
 * and the detent spring, labels on settle. No game view, monitor or sound yet.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <features/features_cpu.h>
#include <retro_miscellaneous.h>

#include "../../config.h"
#include "../../gfx/common/gl2_common.h"
#include "../../gfx/font_driver.h"
#include "../../gfx/gfx_display.h"
#include "../../gfx/video_driver.h"
#include "../../verbosity.h"
#include "../menu_driver.h"

#define RAMEN_FONT_PATH "/usr/share/miroki/fonts/Audiowide-Regular.ttf"

/* Carousel: 9 slots at 40 degrees holding the 3 systems three times over.
 * The selected slot sits at 45 degrees (the screen diagonal), its neighbours
 * at 5 and 85, half clipped by the bottom and right edges. */
#define RAMEN_SLOTS       9
#define RAMEN_SLOT_DEG    40.0f
#define RAMEN_SELECT_DEG  45.0f
#define RAMEN_SYSTEMS     3

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

/* Pivot corner in screen space (origin top-left, y down): the design is
 * written for the open orientation, pivot at the bottom-right. Switch these
 * if the panel comes up with the hinge elsewhere. */
#define RAMEN_PIVOT_X     1.0f
#define RAMEN_PIVOT_Y     1.0f

static const struct
{
   const char *name;
   float rgb[3];
} ramen_systems[RAMEN_SYSTEMS] = {
   { "N64",    { 0.22f, 0.60f, 0.95f } },
   { "PS1",    { 0.55f, 0.55f, 0.60f } },
   { "Saturn", { 0.95f, 0.45f, 0.20f } },
};

typedef struct ramen_handle
{
   /* Wheel physics. wheel is the rotation of slot 0 in degrees; slot k sits
    * at wheel + k * 40. Unwrapped, so it just grows. */
   float wheel;
   float wheel_vel;
   float wheel_target;
   bool boot_spin;       /* friction phase before the spring takes over */
   bool settled;
   retro_time_t last_us;

   /* Confirm jiggle (0 = idle), squash-and-stretch along the tangent. */
   float jiggle;

   /* Label surfacing, 0..1. */
   float label;

   /* GL */
   bool gl_ready;
   GLuint prog;
   GLuint vao;
   GLuint vbo;
   GLint u_size;
   GLint u_pivot;
   GLint u_radius;
   GLint u_sphere_r;
   GLint u_count;
   GLint u_spheres;
   GLint u_colors;

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
 * right edge, increasing clockwise on screen. */
static const char *ramen_frag =
   "#version 300 es\n"
   "precision highp float;\n"
   "in vec2 v_px;\n"
   "out vec4 frag;\n"
   "uniform vec2 u_size;\n"
   "uniform vec2 u_pivot;\n"
   "uniform float u_radius;\n"
   "uniform float u_sphere_r;\n"
   "uniform int u_count;\n"
   "uniform vec4 u_spheres[9];\n"
   "uniform vec3 u_colors[9];\n"
   "\n"
   "void main() {\n"
   "  float S = min(u_size.x, u_size.y);\n"
   "  vec2 rel = v_px - u_pivot;\n"
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
   "    vec2 q = v_px - c;\n"
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
   "    float alpha = edge * (0.82 + 0.18 * z) * (1.0 - stretch * 0.4);\n"
   "    col = mix(col, base, alpha);\n"
   "  }\n"
   "  frag = vec4(col, 1.0);\n"
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
   rm->u_radius   = glGetUniformLocation(rm->prog, "u_radius");
   rm->u_sphere_r = glGetUniformLocation(rm->prog, "u_sphere_r");
   rm->u_count    = glGetUniformLocation(rm->prog, "u_count");
   rm->u_spheres  = glGetUniformLocation(rm->prog, "u_spheres");
   rm->u_colors   = glGetUniformLocation(rm->prog, "u_colors");

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
   /* TODO: land on the last-played system; the slot holding it nearest the
    * selected position is as good as any since the carousel repeats. */
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

static void ramen_update(ramen_handle_t *rm, retro_time_t now)
{
   float dt;

   if (!rm->last_us)
      rm->last_us = now;
   dt          = (float)(now - rm->last_us) * 1e-6f;
   rm->last_us = now;
   if (dt > 0.05f)
      dt = 0.05f;

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
      /* Underdamped detent spring, semi-implicit Euler. */
      float w   = 2.0f * (float)M_PI * RAMEN_SPRING_HZ;
      float acc = w * w * (rm->wheel_target - rm->wheel)
                - 2.0f * RAMEN_SPRING_ZETA * w * rm->wheel_vel;
      rm->wheel_vel += acc * dt;
      rm->wheel     += rm->wheel_vel * dt;

      if (fabsf(rm->wheel_target - rm->wheel) < 0.3f
            && fabsf(rm->wheel_vel) < 8.0f)
      {
         if (!rm->settled)
         {
            rm->settled   = true;
            rm->wheel     = rm->wheel_target;
            rm->wheel_vel = 0.0f;
         }
      }
   }

   /* Labels surface once settled (~250 ms) and sink fast otherwise. */
   if (rm->settled)
      rm->label = MIN(1.0f, rm->label + dt / 0.25f);
   else
      rm->label = MAX(0.0f, rm->label - dt / 0.12f);

   if (rm->jiggle > 0.0f)
      rm->jiggle = MAX(0.0f, rm->jiggle - dt / 0.2f);
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
   ramen_begin_boot_spin(rm, 0);
   return menu;
}

/* RetroArch frees the handle itself after this; only its contents go here. */
static void ramen_free(void *data)
{
   ramen_handle_t *rm = (ramen_handle_t*)data;
   if (!rm)
      return;
   if (rm->font)
      font_driver_free(rm->font);
   rm->font = NULL;
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
   float spheres[RAMEN_SLOTS][4];
   float colors[RAMEN_SLOTS][3];
   int count            = 0;
   int selected_slot;
   int k;

   if (!rm || !rm->gl_ready)
      return;

   ramen_update(rm, cpu_features_get_time_usec());
   selected_slot = ramen_slot_at(rm->wheel_target);

   /* Only the spheres on or near the visible quadrant. */
   for (k = 0; k < RAMEN_SLOTS; k++)
   {
      float deg = fmodf(rm->wheel + k * RAMEN_SLOT_DEG, 360.0f);
      float stretch, dim, scale;

      if (deg < 0.0f)
         deg += 360.0f;
      if (deg > 90.0f + 20.0f && deg < 360.0f - 20.0f)
         continue;

      /* Motion blur: stretch along the tangent with angular speed. */
      stretch = MIN(3.0f, fabsf(rm->wheel_vel) / 400.0f);
      /* Selected slot grows and the rest dim, fading in as it settles. */
      dim     = (k == selected_slot) ? 0.0f : 0.6f * rm->label;
      scale   = 1.0f + ((k == selected_slot) ? 0.15f * rm->label : 0.0f);
      if (k == selected_slot && rm->jiggle > 0.0f)
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
      count++;
   }

   glViewport(0, 0, width, height);
   glDisable(GL_SCISSOR_TEST);
   glDisable(GL_BLEND);
   glUseProgram(rm->prog);
   glBindVertexArray(rm->vao);
   glUniform2f(rm->u_size, (float)width, (float)height);
   glUniform2fv(rm->u_pivot, 1, pivot);
   glUniform1f(rm->u_radius, radius);
   glUniform1f(rm->u_sphere_r, RAMEN_SPHERE_R * S);
   glUniform1i(rm->u_count, count);
   if (count)
   {
      glUniform4fv(rm->u_spheres, count, &spheres[0][0]);
      glUniform3fv(rm->u_colors, count, &colors[0][0]);
   }
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glBindVertexArray(0);
   glUseProgram(0);

   /* The label surfaces through the selected sphere: it rises a few pixels
    * and fades in. (The centre-out reveal and the blur come later, once the
    * text goes through a texture.) The font driver binds its own program
    * and blend state. */
   if (rm->font && rm->label > 0.0f)
   {
      int sys     = ramen_selected_system(rm);
      float a     = RAMEN_SELECT_DEG * (float)M_PI / 180.0f;
      float ease  = 1.0f - (1.0f - rm->label) * (1.0f - rm->label);
      float cx    = pivot[0] - radius * cosf(a);
      float cy    = pivot[1] - radius * sinf(a);
      float rise  = RAMEN_SPHERE_R * S * (0.35f - 0.15f * ease);
      uint32_t color = COLOR_ABGR(255, 255, 255, (unsigned)(ease * 255.0f));

      gfx_display_draw_text(rm->font, ramen_systems[sys].name,
            cx, cy + rise + rm->font_size * 0.35f, width, height,
            color, TEXT_ALIGN_CENTER, 1.0f, false, 0.0f, true);
   }
   glDisable(GL_BLEND);
}

static int ramen_entry_action(void *userdata, menu_entry_t *entry,
      size_t i, enum menu_action action)
{
   ramen_handle_t *rm = (ramen_handle_t*)userdata;

   if (!rm)
      return -1;

   switch (action)
   {
      /* Clockwise lifts the sphere peeking at the bottom edge up and to the
       * right into the selected slot, so it is Right and Up. */
      case MENU_ACTION_LEFT:
      case MENU_ACTION_DOWN:
         ramen_step(rm, -1);
         break;
      case MENU_ACTION_RIGHT:
      case MENU_ACTION_UP:
         ramen_step(rm, 1);
         break;
      case MENU_ACTION_OK:
         if (rm->boot_spin)
            ramen_snap(rm);
         else if (rm->settled)
            rm->jiggle = 1.0f;
         /* TODO: clear-out, launch along the tube and the camera chase
          * into the game view */
         break;
      case MENU_ACTION_CANCEL:
      case MENU_ACTION_START:
      case MENU_ACTION_SELECT:
      case MENU_ACTION_INFO:
      case MENU_ACTION_SEARCH:
      case MENU_ACTION_SCAN:
         if (rm->boot_spin)
            ramen_snap(rm);
         break;
      default:
         /* MENU_ACTION_NOOP arrives every iteration nothing is pressed,
          * so it must not count as input. */
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
   NULL,                        /* render */
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
