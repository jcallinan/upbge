/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup wm
 *
 * \brief Snap cursor.
 */

#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_object.h"
#include "BKE_scene.h"

#include "GPU_immediate.h"
#include "GPU_matrix.h"

#include "ED_screen.h"
#include "ED_transform.h"
#include "ED_transform_snap_object_context.h"
#include "ED_view3d.h"

#include "UI_resources.h"

#include "RNA_access.h"

#include "DEG_depsgraph_query.h"

#include "WM_api.h"

#define STATE_LEN 3

typedef struct SnapStateIntern {
  V3DSnapCursorState snap_state;
  float prevpoint_stack[3];
  int state_active_prev;
  bool is_active;
} SnapStateIntern;

typedef struct SnapCursorDataIntern {
  V3DSnapCursorState state_default;
  SnapStateIntern state_intern[STATE_LEN];
  V3DSnapCursorData snap_data;

  int state_active_len;
  int state_active;

  struct SnapObjectContext *snap_context_v3d;
  const Scene *scene;
  short snap_elem_hidden;

  /* Copy of the parameters of the last event state in order to detect updates. */
  struct {
    int x;
    int y;
#ifdef USE_SNAP_DETECT_FROM_KEYMAP_HACK
    short shift, ctrl, alt, oskey;
#endif
  } last_eventstate;

#ifdef USE_SNAP_DETECT_FROM_KEYMAP_HACK
  struct wmKeyMap *keymap;
  int snap_on;
#endif

  struct wmPaintCursor *handle;

  bool is_initiated;
} SnapCursorDataIntern;

static void UNUSED_FUNCTION(v3d_cursor_snap_state_init)(V3DSnapCursorState *state)
{
  state->prevpoint = NULL;
  state->snap_elem_force = (SCE_SNAP_MODE_VERTEX | SCE_SNAP_MODE_EDGE | SCE_SNAP_MODE_FACE |
                            SCE_SNAP_MODE_EDGE_PERPENDICULAR | SCE_SNAP_MODE_EDGE_MIDPOINT);
  state->plane_axis = 2;
  rgba_uchar_args_set(state->color_point, 255, 255, 255, 255);
  rgba_uchar_args_set(state->color_line, 255, 255, 255, 128);
  state->draw_point = true;
  state->draw_plane = false;
}
static SnapCursorDataIntern g_data_intern = {
    .state_default = {.prevpoint = NULL,
                      .snap_elem_force = (SCE_SNAP_MODE_VERTEX | SCE_SNAP_MODE_EDGE |
                                          SCE_SNAP_MODE_FACE | SCE_SNAP_MODE_EDGE_PERPENDICULAR |
                                          SCE_SNAP_MODE_EDGE_MIDPOINT),
                      .plane_axis = 2,
                      .color_point = {255, 255, 255, 255},
                      .color_line = {255, 255, 255, 128},
                      .draw_point = true,
                      .draw_plane = false}};

/**
 * Calculate a 3x3 orientation matrix from the surface under the cursor.
 */
static void v3d_cursor_poject_surface_normal(const float normal[3],
                                             const float obmat[4][4],
                                             float r_mat[3][3])
{
  float mat[3][3];
  copy_m3_m4(mat, obmat);
  normalize_m3(mat);

  float dot_best = fabsf(dot_v3v3(mat[0], normal));
  int i_best = 0;
  for (int i = 1; i < 3; i++) {
    float dot_test = fabsf(dot_v3v3(mat[i], normal));
    if (dot_test > dot_best) {
      i_best = i;
      dot_best = dot_test;
    }
  }
  if (dot_v3v3(mat[i_best], normal) < 0.0f) {
    negate_v3(mat[(i_best + 1) % 3]);
    negate_v3(mat[(i_best + 2) % 3]);
  }
  copy_v3_v3(mat[i_best], normal);
  orthogonalize_m3(mat, i_best);
  normalize_m3(mat);

  copy_v3_v3(r_mat[0], mat[(i_best + 1) % 3]);
  copy_v3_v3(r_mat[1], mat[(i_best + 2) % 3]);
  copy_v3_v3(r_mat[2], mat[i_best]);
}

/**
 * Calculate 3D view incremental (grid) snapping.
 *
 * \note This could be moved to a public function.
 */
static bool v3d_cursor_snap_calc_incremental(
    Scene *scene, View3D *v3d, ARegion *region, const float co_relative[3], float co[3])
{
  const float grid_size = ED_view3d_grid_view_scale(scene, v3d, region, NULL);
  if (UNLIKELY(grid_size == 0.0f)) {
    return false;
  }

  if (scene->toolsettings->snap_flag & SCE_SNAP_ABS_GRID) {
    co_relative = NULL;
  }

  if (co_relative != NULL) {
    sub_v3_v3(co, co_relative);
  }
  mul_v3_fl(co, 1.0f / grid_size);
  co[0] = roundf(co[0]);
  co[1] = roundf(co[1]);
  co[2] = roundf(co[2]);
  mul_v3_fl(co, grid_size);
  if (co_relative != NULL) {
    add_v3_v3(co, co_relative);
  }

  return true;
}

/**
 * Re-order \a mat so \a axis_align uses its own axis which is closest to \a v.
 */
static bool mat3_align_axis_to_v3(float mat[3][3], const int axis_align, const float v[3])
{
  float dot_best = -1.0f;
  int axis_found = axis_align;
  for (int i = 0; i < 3; i++) {
    const float dot_test = fabsf(dot_v3v3(mat[i], v));
    if (dot_test > dot_best) {
      dot_best = dot_test;
      axis_found = i;
    }
  }

  if (axis_align != axis_found) {
    float tmat[3][3];
    copy_m3_m3(tmat, mat);
    const int offset = mod_i(axis_found - axis_align, 3);
    for (int i = 0; i < 3; i++) {
      copy_v3_v3(mat[i], tmat[(i + offset) % 3]);
    }
    return true;
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name Drawings
 * \{ */

static void v3d_cursor_plane_draw_grid(const int resolution,
                                       const float scale,
                                       const float scale_fade,
                                       const float matrix[4][4],
                                       const int plane_axis,
                                       const float color[4])
{
  BLI_assert(scale_fade <= scale);
  const int resolution_min = resolution - 1;
  float color_fade[4] = {UNPACK4(color)};
  const float *center = matrix[3];

  GPU_blend(GPU_BLEND_ADDITIVE);
  GPU_line_smooth(true);
  GPU_line_width(1.0f);

  GPUVertFormat *format = immVertexFormat();
  const uint pos_id = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  const uint col_id = GPU_vertformat_attr_add(format, "color", GPU_COMP_F32, 4, GPU_FETCH_FLOAT);

  immBindBuiltinProgram(GPU_SHADER_3D_SMOOTH_COLOR);

  const size_t coords_len = resolution * resolution;
  float(*coords)[3] = MEM_mallocN(sizeof(*coords) * coords_len, __func__);

  const int axis_x = (plane_axis + 0) % 3;
  const int axis_y = (plane_axis + 1) % 3;
  const int axis_z = (plane_axis + 2) % 3;

  int i;
  const float resolution_div = (float)1.0f / (float)resolution;
  i = 0;
  for (int x = 0; x < resolution; x++) {
    const float x_fl = (x * resolution_div) - 0.5f;
    for (int y = 0; y < resolution; y++) {
      const float y_fl = (y * resolution_div) - 0.5f;
      coords[i][axis_x] = 0.0f;
      coords[i][axis_y] = x_fl * scale;
      coords[i][axis_z] = y_fl * scale;
      mul_m4_v3(matrix, coords[i]);
      i += 1;
    }
  }
  BLI_assert(i == (int)coords_len);
  immBeginAtMost(GPU_PRIM_LINES, coords_len * 4);
  i = 0;
  for (int x = 0; x < resolution_min; x++) {
    for (int y = 0; y < resolution_min; y++) {

      /* Add #resolution_div to ensure we fade-out entirely. */
#define FADE(v) \
  max_ff(0.0f, (1.0f - square_f(((len_v3v3(v, center) / scale_fade) + resolution_div) * 2.0f)))

      const float *v0 = coords[(resolution * x) + y];
      const float *v1 = coords[(resolution * (x + 1)) + y];
      const float *v2 = coords[(resolution * x) + (y + 1)];

      const float f0 = FADE(v0);
      const float f1 = FADE(v1);
      const float f2 = FADE(v2);

      if (f0 > 0.0f || f1 > 0.0f) {
        color_fade[3] = color[3] * f0;
        immAttr4fv(col_id, color_fade);
        immVertex3fv(pos_id, v0);
        color_fade[3] = color[3] * f1;
        immAttr4fv(col_id, color_fade);
        immVertex3fv(pos_id, v1);
      }
      if (f0 > 0.0f || f2 > 0.0f) {
        color_fade[3] = color[3] * f0;
        immAttr4fv(col_id, color_fade);
        immVertex3fv(pos_id, v0);

        color_fade[3] = color[3] * f2;
        immAttr4fv(col_id, color_fade);
        immVertex3fv(pos_id, v2);
      }

#undef FADE

      i++;
    }
  }

  MEM_freeN(coords);

  immEnd();

  immUnbindProgram();

  GPU_line_smooth(false);
  GPU_blend(GPU_BLEND_NONE);
}

static void v3d_cursor_plane_draw(const RegionView3D *rv3d,
                                  const int plane_axis,
                                  const float matrix[4][4])
{
  /* Draw */
  float pixel_size;

  if (rv3d->is_persp) {
    float center[3];
    negate_v3_v3(center, rv3d->ofs);
    pixel_size = ED_view3d_pixel_size(rv3d, center);
  }
  else {
    pixel_size = ED_view3d_pixel_size(rv3d, matrix[3]);
  }

  if (pixel_size > FLT_EPSILON) {

    /* Arbitrary, 1.0 is a little too strong though. */
    float color_alpha = 0.75f;
    if (rv3d->is_persp) {
      /* Scale down the alpha when this is drawn very small,
       * since the add shader causes the small size to show too dense & bright. */
      const float relative_pixel_scale = pixel_size / ED_view3d_pixel_size(rv3d, matrix[3]);
      if (relative_pixel_scale < 1.0f) {
        color_alpha *= max_ff(square_f(relative_pixel_scale), 0.3f);
      }
    }

    {
      /* Extra adjustment when it's near view-aligned as it seems overly bright. */
      float view_vector[3];
      ED_view3d_global_to_vector(rv3d, matrix[3], view_vector);
      float view_dot = fabsf(dot_v3v3(matrix[plane_axis], view_vector));
      color_alpha *= max_ff(0.3f, 1.0f - square_f(square_f(1.0f - view_dot)));
    }

    const float scale_mod = U.gizmo_size * 2 * U.dpi_fac / U.pixelsize;

    float final_scale = (scale_mod * pixel_size);

    const int lines_subdiv = 10;
    int lines = lines_subdiv;

    float final_scale_fade = final_scale;
    final_scale = ceil_power_of_10(final_scale);

    float fac = final_scale_fade / final_scale;

    float color[4] = {1, 1, 1, color_alpha};
    color[3] *= square_f(1.0f - fac);
    if (color[3] > 0.0f) {
      v3d_cursor_plane_draw_grid(
          lines * lines_subdiv, final_scale, final_scale_fade, matrix, plane_axis, color);
    }

    color[3] = color_alpha;
    /* When the grid is large, we only need the 2x lines in the middle. */
    if (fac < 0.2f) {
      lines = 1;
      final_scale = final_scale_fade;
    }
    v3d_cursor_plane_draw_grid(lines, final_scale, final_scale_fade, matrix, plane_axis, color);
  }
}

void ED_view3d_cursor_snap_draw_util(RegionView3D *rv3d,
                                     const float loc_prev[3],
                                     const float loc_curr[3],
                                     const float normal[3],
                                     const uchar color_line[4],
                                     const uchar color_point[4],
                                     const short snap_elem_type)
{
  if (!loc_prev && !loc_curr) {
    return;
  }

  float view_inv[4][4];
  copy_m4_m4(view_inv, rv3d->viewinv);

  /* The size of the circle is larger than the vertex size.
   * This prevents a drawing overlaps the other. */
  float radius = 2.5f * UI_GetThemeValuef(TH_VERTEX_SIZE);
  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  if (loc_curr) {
    immUniformColor4ubv(color_point);
    imm_drawcircball(loc_curr, ED_view3d_pixel_size(rv3d, loc_curr) * radius, view_inv, pos);

    /* draw normal if needed */
    if (normal) {
      immBegin(GPU_PRIM_LINES, 2);
      immVertex3fv(pos, loc_curr);
      immVertex3f(pos, loc_curr[0] + normal[0], loc_curr[1] + normal[1], loc_curr[2] + normal[2]);
      immEnd();
    }
  }

  if (loc_prev) {
    /* Draw an "X" indicating where the previous snap point is.
     * This is useful for indicating perpendicular snap. */

    /* v1, v2, v3 and v4 indicate the coordinates of the ends of the "X". */
    float vx[3], vy[3], v1[3], v2[3], v3[3], v4[4];

    /* Multiply by 0.75f so that the final size of the "X" is close to that of
     * the circle.
     * (A closer value is 0.7071f, but we don't need to be exact here). */
    float x_size = 0.75f * radius * ED_view3d_pixel_size(rv3d, loc_prev);

    mul_v3_v3fl(vx, view_inv[0], x_size);
    mul_v3_v3fl(vy, view_inv[1], x_size);

    add_v3_v3v3(v1, vx, vy);
    sub_v3_v3v3(v2, vx, vy);
    negate_v3_v3(v3, v1);
    negate_v3_v3(v4, v2);

    add_v3_v3(v1, loc_prev);
    add_v3_v3(v2, loc_prev);
    add_v3_v3(v3, loc_prev);
    add_v3_v3(v4, loc_prev);

    immUniformColor4ubv(color_line);
    immBegin(GPU_PRIM_LINES, 4);
    immVertex3fv(pos, v3);
    immVertex3fv(pos, v1);
    immVertex3fv(pos, v4);
    immVertex3fv(pos, v2);
    immEnd();

    if (loc_curr && (snap_elem_type & SCE_SNAP_MODE_EDGE_PERPENDICULAR)) {
      /* Dashed line. */
      immUnbindProgram();

      immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);
      float viewport_size[4];
      GPU_viewport_size_get_f(viewport_size);
      immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);
      immUniform1f("dash_width", 6.0f * U.pixelsize);
      immUniform1f("dash_factor", 1.0f / 4.0f);
      immUniformColor4ubv(color_line);

      immBegin(GPU_PRIM_LINES, 2);
      immVertex3fv(pos, loc_prev);
      immVertex3fv(pos, loc_curr);
      immEnd();
    }
  }

  immUnbindProgram();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Event State
 * \{ */

/* Checks if the current event is different from the one captured in the last update. */
static bool v3d_cursor_eventstate_has_changed(SnapCursorDataIntern *data_intern,
                                              V3DSnapCursorState *state,
                                              const wmWindowManager *wm,
                                              const int x,
                                              const int y)
{
  if (wm && wm->winactive) {
    const wmEvent *event = wm->winactive->eventstate;
    if ((x != data_intern->last_eventstate.x) || (y != data_intern->last_eventstate.y)) {
      return true;
    }
#ifdef USE_SNAP_DETECT_FROM_KEYMAP_HACK
    if (!(state && (state->flag & V3D_SNAPCURSOR_TOGGLE_ALWAYS_TRUE))) {
      if ((event->ctrl != data_intern->last_eventstate.ctrl) ||
          (event->shift != data_intern->last_eventstate.shift) ||
          (event->alt != data_intern->last_eventstate.alt) ||
          (event->oskey != data_intern->last_eventstate.oskey)) {
        return true;
      }
    }
#endif
  }
  return false;
}

/* Copies the current eventstate. */
static void v3d_cursor_eventstate_save_xy(SnapCursorDataIntern *cursor_snap,
                                          const int x,
                                          const int y)
{
  cursor_snap->last_eventstate.x = x;
  cursor_snap->last_eventstate.y = y;
}

#ifdef USE_SNAP_DETECT_FROM_KEYMAP_HACK
static bool v3d_cursor_is_snap_invert(SnapCursorDataIntern *data_intern, const wmWindowManager *wm)
{
  if (!wm || !wm->winactive) {
    return false;
  }

  const wmEvent *event = wm->winactive->eventstate;
  if ((event->ctrl == data_intern->last_eventstate.ctrl) &&
      (event->shift == data_intern->last_eventstate.shift) &&
      (event->alt == data_intern->last_eventstate.alt) &&
      (event->oskey == data_intern->last_eventstate.oskey)) {
    /* Nothing has changed. */
    return data_intern->snap_data.is_snap_invert;
  }

  /* Save new eventstate. */
  data_intern->last_eventstate.ctrl = event->ctrl;
  data_intern->last_eventstate.shift = event->shift;
  data_intern->last_eventstate.alt = event->alt;
  data_intern->last_eventstate.oskey = event->oskey;

  const int snap_on = data_intern->snap_on;

  wmKeyMap *keymap = WM_keymap_active(wm, data_intern->keymap);
  for (wmKeyMapItem *kmi = keymap->items.first; kmi; kmi = kmi->next) {
    if (kmi->flag & KMI_INACTIVE) {
      continue;
    }

    if (kmi->propvalue == snap_on) {
      if ((ELEM(kmi->type, EVT_LEFTCTRLKEY, EVT_RIGHTCTRLKEY) && event->ctrl) ||
          (ELEM(kmi->type, EVT_LEFTSHIFTKEY, EVT_RIGHTSHIFTKEY) && event->shift) ||
          (ELEM(kmi->type, EVT_LEFTALTKEY, EVT_RIGHTALTKEY) && event->alt) ||
          ((kmi->type == EVT_OSKEY) && event->oskey)) {
        return true;
      }
    }
  }
  return false;
}
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Update
 * \{ */

static short v3d_cursor_snap_elements(V3DSnapCursorState *snap_state, Scene *scene)
{
  short snap_elements = snap_state->snap_elem_force;
  if (!snap_elements) {
    return scene->toolsettings->snap_mode;
  }
  return snap_elements;
}

static void v3d_cursor_snap_context_ensure(Scene *scene)
{
  SnapCursorDataIntern *data_intern = &g_data_intern;
  if (data_intern->snap_context_v3d && (data_intern->scene != scene)) {
    ED_transform_snap_object_context_destroy(data_intern->snap_context_v3d);
    data_intern->snap_context_v3d = NULL;
  }
  if (data_intern->snap_context_v3d == NULL) {
    data_intern->snap_context_v3d = ED_transform_snap_object_context_create(scene, 0);
    data_intern->scene = scene;
  }
}

static void v3d_cursor_snap_update(V3DSnapCursorState *state,
                                   const bContext *C,
                                   wmWindowManager *wm,
                                   Depsgraph *depsgraph,
                                   Scene *scene,
                                   ARegion *region,
                                   View3D *v3d,
                                   int x,
                                   int y)
{
  SnapCursorDataIntern *data_intern = &g_data_intern;
  V3DSnapCursorData *snap_data = &data_intern->snap_data;
  v3d_cursor_snap_context_ensure(scene);

  float co[3], no[3], face_nor[3], obmat[4][4], omat[3][3];
  short snap_elem = 0;
  int snap_elem_index[3] = {-1, -1, -1};
  int index = -1;

  const float mval_fl[2] = {x, y};
  zero_v3(no);
  zero_v3(face_nor);
  unit_m3(omat);

  ushort snap_elements = v3d_cursor_snap_elements(state, scene);
  data_intern->snap_elem_hidden = 0;
  const bool draw_plane = state->draw_plane;
  if (draw_plane && !(snap_elements & SCE_SNAP_MODE_FACE)) {
    data_intern->snap_elem_hidden = SCE_SNAP_MODE_FACE;
    snap_elements |= SCE_SNAP_MODE_FACE;
  }

  snap_data->is_enabled = true;
#ifdef USE_SNAP_DETECT_FROM_KEYMAP_HACK
  if (!(state->flag & V3D_SNAPCURSOR_TOGGLE_ALWAYS_TRUE)) {
    snap_data->is_snap_invert = v3d_cursor_is_snap_invert(data_intern, wm);

    const ToolSettings *ts = scene->toolsettings;
    if (snap_data->is_snap_invert != !(ts->snap_flag & SCE_SNAP)) {
      snap_data->is_enabled = false;
      if (!draw_plane) {
        snap_data->snap_elem = 0;
        return;
      }
      snap_elements = data_intern->snap_elem_hidden = SCE_SNAP_MODE_FACE;
    }
  }
#endif

  if (snap_elements & (SCE_SNAP_MODE_VERTEX | SCE_SNAP_MODE_EDGE | SCE_SNAP_MODE_FACE |
                       SCE_SNAP_MODE_EDGE_MIDPOINT | SCE_SNAP_MODE_EDGE_PERPENDICULAR)) {
    float prev_co[3] = {0.0f};
    if (state->prevpoint) {
      copy_v3_v3(prev_co, state->prevpoint);
    }
    else {
      snap_elements &= ~SCE_SNAP_MODE_EDGE_PERPENDICULAR;
    }

    eSnapSelect snap_select = (state->flag & V3D_SNAPCURSOR_SNAP_ONLY_ACTIVE) ? SNAP_ONLY_ACTIVE :
                                                                                SNAP_ALL;

    eSnapEditType edit_mode_type = (state->flag & V3D_SNAPCURSOR_SNAP_EDIT_GEOM_FINAL) ?
                                       SNAP_GEOM_FINAL :
                                   (state->flag & V3D_SNAPCURSOR_SNAP_EDIT_GEOM_CAGE) ?
                                       SNAP_GEOM_CAGE :
                                       SNAP_GEOM_EDIT;

    bool use_occlusion_test = (state->flag & V3D_SNAPCURSOR_OCCLUSION_ALWAYS_TRUE) ? false : true;

    float dist_px = 12.0f * U.pixelsize;

    snap_elem = ED_transform_snap_object_project_view3d_ex(
        data_intern->snap_context_v3d,
        depsgraph,
        region,
        v3d,
        snap_elements,
        &(const struct SnapObjectParams){
            .snap_select = snap_select,
            .edit_mode_type = edit_mode_type,
            .use_occlusion_test = use_occlusion_test,
        },
        mval_fl,
        prev_co,
        &dist_px,
        co,
        no,
        &index,
        NULL,
        obmat,
        face_nor);
  }

  if (is_zero_v3(face_nor)) {
    face_nor[state->plane_axis] = 1.0f;
  }

  if (draw_plane) {
    bool orient_surface = snap_elem && (state->plane_orient == V3D_PLACE_ORIENT_SURFACE);
    if (orient_surface) {
      copy_m3_m4(omat, obmat);
    }
    else {
      ViewLayer *view_layer = CTX_data_view_layer(C);
      Object *ob = OBACT(view_layer);
      const int orient_index = BKE_scene_orientation_get_index(scene, SCE_ORIENT_DEFAULT);
      const int pivot_point = scene->toolsettings->transform_pivot_point;
      ED_transform_calc_orientation_from_type_ex(
          scene, view_layer, v3d, region->regiondata, ob, ob, orient_index, pivot_point, omat);

      RegionView3D *rv3d = region->regiondata;
      if (state->use_plane_axis_auto) {
        mat3_align_axis_to_v3(omat, state->plane_axis, rv3d->viewinv[2]);
      }
    }

    /* Non-orthogonal matrices cause the preview and final result not to match.
     *
     * While making orthogonal doesn't always work well (especially with gimbal orientation for
     * e.g.) it's a corner case, without better alternatives as objects don't support shear. */
    orthogonalize_m3(omat, state->plane_axis);

    if (orient_surface) {
      v3d_cursor_poject_surface_normal(face_nor, obmat, omat);
    }
  }

  float *co_depth = snap_elem ? co : scene->cursor.location;
  snap_elem &= ~data_intern->snap_elem_hidden;
  if (snap_elem == 0) {
    float plane[4];
    if (state->plane_depth != V3D_PLACE_DEPTH_CURSOR_VIEW) {
      const float *plane_normal = omat[state->plane_axis];
      plane_from_point_normal_v3(plane, co_depth, plane_normal);
    }

    if ((state->plane_depth == V3D_PLACE_DEPTH_CURSOR_VIEW) ||
        !ED_view3d_win_to_3d_on_plane(region, plane, mval_fl, true, co)) {
      ED_view3d_win_to_3d(v3d, region, co_depth, mval_fl, co);
    }

    if (snap_data->is_enabled && (snap_elements & SCE_SNAP_MODE_INCREMENT)) {
      v3d_cursor_snap_calc_incremental(scene, v3d, region, state->prevpoint, co);
    }
  }
  else if (snap_elem == SCE_SNAP_MODE_VERTEX) {
    snap_elem_index[0] = index;
  }
  else if (snap_elem &
           (SCE_SNAP_MODE_EDGE | SCE_SNAP_MODE_EDGE_MIDPOINT | SCE_SNAP_MODE_EDGE_PERPENDICULAR)) {
    snap_elem_index[1] = index;
  }
  else if (snap_elem == SCE_SNAP_MODE_FACE) {
    snap_elem_index[2] = index;
  }

  snap_data->snap_elem = snap_elem;
  copy_v3_v3(snap_data->loc, co);
  copy_v3_v3(snap_data->nor, no);
  copy_m4_m4(snap_data->obmat, obmat);
  copy_v3_v3_int(snap_data->elem_index, snap_elem_index);

  copy_m3_m3(snap_data->plane_omat, omat);

  v3d_cursor_eventstate_save_xy(data_intern, x, y);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Callbacks
 * \{ */

static bool v3d_cursor_snap_pool_fn(bContext *C)
{
  if (G.moving) {
    return false;
  }

  ARegion *region = CTX_wm_region(C);
  if (region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }

  ScrArea *area = CTX_wm_area(C);
  if (area->spacetype != SPACE_VIEW3D) {
    return false;
  }

  RegionView3D *rv3d = region->regiondata;
  if (rv3d->rflag & RV3D_NAVIGATING) {
    /* Don't draw the cursor while navigating. It can be distracting. */
    return false;
  };

  return true;
}

static void v3d_cursor_snap_draw_fn(bContext *C, int x, int y, void *UNUSED(customdata))
{
  SnapCursorDataIntern *data_intern = &g_data_intern;
  V3DSnapCursorState *state = ED_view3d_cursor_snap_state_get();
  V3DSnapCursorData *snap_data = &data_intern->snap_data;

  wmWindowManager *wm = CTX_wm_manager(C);
  ARegion *region = CTX_wm_region(C);
  x -= region->winrct.xmin;
  y -= region->winrct.ymin;
  if (v3d_cursor_eventstate_has_changed(data_intern, state, wm, x, y)) {
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    Scene *scene = DEG_get_input_scene(depsgraph);
    View3D *v3d = CTX_wm_view3d(C);
    v3d_cursor_snap_update(state, C, wm, depsgraph, scene, region, v3d, x, y);
  }

  const bool draw_plane = state->draw_plane;
  if (!snap_data->snap_elem && !draw_plane) {
    return;
  }

  /* Setup viewport & matrix. */
  RegionView3D *rv3d = region->regiondata;
  wmViewport(&region->winrct);
  GPU_matrix_projection_set(rv3d->winmat);
  GPU_matrix_set(rv3d->viewmat);

  GPU_blend(GPU_BLEND_ALPHA);

  float matrix[4][4];
  if (draw_plane) {
    copy_m4_m3(matrix, snap_data->plane_omat);
    copy_v3_v3(matrix[3], snap_data->loc);

    v3d_cursor_plane_draw(rv3d, state->plane_axis, matrix);
  }

  if (snap_data->snap_elem && state->draw_point) {
    const float *prev_point = (snap_data->snap_elem & SCE_SNAP_MODE_EDGE_PERPENDICULAR) ?
                                  state->prevpoint :
                                  NULL;

    GPU_line_smooth(false);
    GPU_line_width(1.0f);

    ED_view3d_cursor_snap_draw_util(rv3d,
                                    prev_point,
                                    snap_data->loc,
                                    NULL,
                                    state->color_line,
                                    state->color_point,
                                    snap_data->snap_elem);
  }

  GPU_blend(GPU_BLEND_NONE);

  /* Restore matrix. */
  wmWindowViewport(CTX_wm_window(C));
}

/** \} */

V3DSnapCursorState *ED_view3d_cursor_snap_state_get(void)
{
  if (!g_data_intern.state_active_len) {
    return &g_data_intern.state_default;
  }
  return (V3DSnapCursorState *)&g_data_intern.state_intern[g_data_intern.state_active];
}

static void v3d_cursor_snap_activate(void)
{
  SnapCursorDataIntern *data_intern = &g_data_intern;

  if (!data_intern->handle) {
    if (!data_intern->is_initiated) {
      /* Only initiate intern data once.
       * TODO: ED_view3d_cursor_snap_init */

#ifdef USE_SNAP_DETECT_FROM_KEYMAP_HACK
      struct wmKeyConfig *keyconf = ((wmWindowManager *)G.main->wm.first)->defaultconf;

      data_intern->keymap = WM_modalkeymap_find(keyconf, "Generic Gizmo Tweak Modal Map");
      RNA_enum_value_from_id(data_intern->keymap->modal_items, "SNAP_ON", &data_intern->snap_on);
#endif
      data_intern->is_initiated = true;
    }

    struct wmPaintCursor *pc = WM_paint_cursor_activate(
        SPACE_VIEW3D, RGN_TYPE_WINDOW, v3d_cursor_snap_pool_fn, v3d_cursor_snap_draw_fn, NULL);
    data_intern->handle = pc;
  }
}

static void v3d_cursor_snap_free(void)
{
  SnapCursorDataIntern *data_intern = &g_data_intern;
  if (data_intern->handle && G_MAIN->wm.first) {
    WM_paint_cursor_end(data_intern->handle);
    data_intern->handle = NULL;
  }
  if (data_intern->snap_context_v3d) {
    ED_transform_snap_object_context_destroy(data_intern->snap_context_v3d);
    data_intern->snap_context_v3d = NULL;
  }

  for (SnapStateIntern *state_intern = data_intern->state_intern;
       state_intern < &data_intern->state_intern[STATE_LEN];
       state_intern++) {
    state_intern->is_active = false;
  }
}

void ED_view3d_cursor_snap_state_default_set(V3DSnapCursorState *state)
{
  g_data_intern.state_default = *state;
}

V3DSnapCursorState *ED_view3d_cursor_snap_active(void)
{
  SnapCursorDataIntern *data_intern = &g_data_intern;
  if (!data_intern->state_active_len) {
    v3d_cursor_snap_activate();
  }

  data_intern->state_active_len++;
  for (int i = 0; i < STATE_LEN; i++) {
    SnapStateIntern *state_intern = &g_data_intern.state_intern[i];
    if (!state_intern->is_active) {
      state_intern->snap_state = g_data_intern.state_default;
      state_intern->is_active = true;
      state_intern->state_active_prev = data_intern->state_active;
      data_intern->state_active = i;
      return (V3DSnapCursorState *)state_intern;
    }
  }

  BLI_assert(false);
  data_intern->state_active_len--;
  return NULL;
}

void ED_view3d_cursor_snap_deactive(V3DSnapCursorState *state)
{
  SnapCursorDataIntern *data_intern = &g_data_intern;
  if (!data_intern->state_active_len) {
    BLI_assert(false);
    return;
  }

  SnapStateIntern *state_intern = (SnapStateIntern *)state;
  if (!state_intern->is_active) {
    return;
  }

  state_intern->is_active = false;
  data_intern->state_active_len--;
  if (!data_intern->state_active_len) {
    v3d_cursor_snap_free();
  }
  else {
    data_intern->state_active = state_intern->state_active_prev;
  }
}

void ED_view3d_cursor_snap_prevpoint_set(V3DSnapCursorState *state, const float prev_point[3])
{
  SnapStateIntern *state_intern = (SnapStateIntern *)state;
  if (prev_point) {
    copy_v3_v3(state_intern->prevpoint_stack, prev_point);
    state->prevpoint = state_intern->prevpoint_stack;
  }
  else {
    state->prevpoint = NULL;
  }
}

V3DSnapCursorData *ED_view3d_cursor_snap_data_get(V3DSnapCursorState *state,
                                                  const bContext *C,
                                                  const int x,
                                                  const int y)
{
  SnapCursorDataIntern *data_intern = &g_data_intern;
  if (C && data_intern->state_active_len) {
    wmWindowManager *wm = CTX_wm_manager(C);
    if (v3d_cursor_eventstate_has_changed(data_intern, state, wm, x, y)) {
      Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
      Scene *scene = DEG_get_input_scene(depsgraph);
      ARegion *region = CTX_wm_region(C);
      View3D *v3d = CTX_wm_view3d(C);

      if (!state) {
        state = ED_view3d_cursor_snap_state_get();
      }
      v3d_cursor_snap_update(state, C, wm, depsgraph, scene, region, v3d, x, y);
    }
  }

  return &data_intern->snap_data;
}

struct SnapObjectContext *ED_view3d_cursor_snap_context_ensure(Scene *scene)
{
  SnapCursorDataIntern *data_intern = &g_data_intern;
  v3d_cursor_snap_context_ensure(scene);
  return data_intern->snap_context_v3d;
}

void ED_view3d_cursor_snap_exit(void)
{
  v3d_cursor_snap_free();
}
