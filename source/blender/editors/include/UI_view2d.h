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
 * The Original Code is Copyright (C) 2008 Blender Foundation.
 * All rights reserved.
 * Generic 2d view with should allow drawing grids,
 * panning, zooming, scrolling, ..
 */

/** \file
 * \ingroup editorui
 */

#pragma once

#include "BLI_compiler_attrs.h"
#include "BLI_rect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------ */
/* Settings and Defines:                      */

/* ---- General Defines ---- */

/* generic value to use when coordinate lies out of view when converting */
#define V2D_IS_CLIPPED 12000

/* Common View2D view types
 * NOTE: only define a type here if it completely sets all (+/- a few) of the relevant flags
 *       and settings for a View2D region, and that set of settings is used in more
 *       than one specific place
 */
enum eView2D_CommonViewTypes {
  /* custom view type (region has defined all necessary flags already) */
  V2D_COMMONVIEW_CUSTOM = -1,
  /* standard (only use this when setting up a new view, as a sensible base for most settings) */
  V2D_COMMONVIEW_STANDARD,
  /* listview (i.e. Outliner) */
  V2D_COMMONVIEW_LIST,
  /* Stack-view (this is basically a list where new items are added at the top). */
  V2D_COMMONVIEW_STACK,
  /* headers (this is basically the same as listview, but no y-panning) */
  V2D_COMMONVIEW_HEADER,
  /* ui region containing panels */
  V2D_COMMONVIEW_PANELS_UI,
};

/* ---- Defines for Scroller Arguments ----- */

/* ------ Defines for Scrollers ----- */

/** Scroll bar area. */
#define V2D_SCROLL_HEIGHT (0.45f * U.widget_unit)
#define V2D_SCROLL_WIDTH (0.45f * U.widget_unit)
/** Scroll bars with 'handles' used for scale (zoom). */
#define V2D_SCROLL_HANDLE_HEIGHT (0.6f * U.widget_unit)
#define V2D_SCROLL_HANDLE_WIDTH (0.6f * U.widget_unit)

/** Scroll bar with 'handles' hot-spot radius for cursor proximity. */
#define V2D_SCROLL_HANDLE_SIZE_HOTSPOT (0.6f * U.widget_unit)

/** Don't allow scroll thumb to show below this size (so it's never too small to click on). */
#define V2D_SCROLL_THUMB_SIZE_MIN (30.0 * UI_DPI_FAC)

/* ------ Define for UI_view2d_sync ----- */

/* means copy it from another v2d */
#define V2D_LOCK_SET 0
/* means copy it to the other v2ds */
#define V2D_LOCK_COPY 1

/* ------------------------------------------ */
/* Macros:                                    */

/* test if mouse in a scrollbar (assume that scroller availability has been tested) */
#define IN_2D_VERT_SCROLL(v2d, co) (BLI_rcti_isect_pt_v(&v2d->vert, co))
#define IN_2D_HORIZ_SCROLL(v2d, co) (BLI_rcti_isect_pt_v(&v2d->hor, co))

#define IN_2D_VERT_SCROLL_RECT(v2d, rct) (BLI_rcti_isect(&v2d->vert, rct, NULL))
#define IN_2D_HORIZ_SCROLL_RECT(v2d, rct) (BLI_rcti_isect(&v2d->hor, rct, NULL))

/* ------------------------------------------ */
/* Type definitions:                          */

struct View2D;
struct View2DScrollers;

struct ARegion;
struct Scene;
struct ScrArea;
struct bContext;
struct bScreen;
struct rctf;
struct rcti;
struct wmEvent;
struct wmGizmoGroupType;
struct wmKeyConfig;
struct wmOperator;
struct wmOperatorType;

typedef struct View2DScrollers View2DScrollers;

/* ----------------------------------------- */
/* Prototypes:                               */

/* refresh and validation (of view rects) */
void UI_view2d_region_reinit(struct View2D *v2d, short type, int winx, int winy);

void UI_view2d_curRect_validate(struct View2D *v2d);
void UI_view2d_curRect_reset(struct View2D *v2d);
bool UI_view2d_area_supports_sync(struct ScrArea *area);
void UI_view2d_sync(struct bScreen *screen, struct ScrArea *area, struct View2D *v2dcur, int flag);

/* Perform all required updates after `v2d->cur` as been modified.
 * This includes like validation view validation (#UI_view2d_curRect_validate).
 *
 * Current intent is to use it from user code, such as view navigation and zoom operations. */
void UI_view2d_curRect_changed(const struct bContext *C, struct View2D *v2d);

void UI_view2d_totRect_set(struct View2D *v2d, int width, int height);
void UI_view2d_totRect_set_resize(struct View2D *v2d, int width, int height, bool resize);

void UI_view2d_mask_from_win(const struct View2D *v2d, struct rcti *r_mask);

void UI_view2d_zoom_cache_reset(void);

/* view matrix operations */
void UI_view2d_view_ortho(const struct View2D *v2d);
void UI_view2d_view_orthoSpecial(struct ARegion *region, struct View2D *v2d, const bool xaxis);
void UI_view2d_view_restore(const struct bContext *C);

/* grid drawing */
void UI_view2d_multi_grid_draw(
    const struct View2D *v2d, int colorid, float step, int level_size, int totlevels);

void UI_view2d_draw_lines_y__values(const struct View2D *v2d);
void UI_view2d_draw_lines_x__values(const struct View2D *v2d);
void UI_view2d_draw_lines_x__discrete_values(const struct View2D *v2d, bool display_minor_lines);
void UI_view2d_draw_lines_x__discrete_time(const struct View2D *v2d,
                                           const struct Scene *scene,
                                           bool display_minor_lines);
void UI_view2d_draw_lines_x__discrete_frames_or_seconds(const struct View2D *v2d,
                                                        const struct Scene *scene,
                                                        bool display_seconds,
                                                        bool display_minor_lines);
void UI_view2d_draw_lines_x__frames_or_seconds(const struct View2D *v2d,
                                               const struct Scene *scene,
                                               bool display_seconds);

float UI_view2d_grid_resolution_x__frames_or_seconds(const struct View2D *v2d,
                                                     const struct Scene *scene,
                                                     bool display_seconds);
float UI_view2d_grid_resolution_y__values(const struct View2D *v2d);

/* scale indicator text drawing */
void UI_view2d_draw_scale_y__values(const struct ARegion *region,
                                    const struct View2D *v2d,
                                    const struct rcti *rect,
                                    int colorid);
void UI_view2d_draw_scale_y__block(const struct ARegion *region,
                                   const struct View2D *v2d,
                                   const struct rcti *rect,
                                   int colorid);
void UI_view2d_draw_scale_x__discrete_frames_or_seconds(const struct ARegion *region,
                                                        const struct View2D *v2d,
                                                        const struct rcti *rect,
                                                        const struct Scene *scene,
                                                        bool display_seconds,
                                                        int colorid);
void UI_view2d_draw_scale_x__frames_or_seconds(const struct ARegion *region,
                                               const struct View2D *v2d,
                                               const struct rcti *rect,
                                               const struct Scene *scene,
                                               bool display_seconds,
                                               int colorid);

/* scrollbar drawing */
void UI_view2d_scrollers_calc(struct View2D *v2d,
                              const struct rcti *mask_custom,
                              struct View2DScrollers *r_scrollers);
void UI_view2d_scrollers_draw(struct View2D *v2d, const struct rcti *mask_custom);

/* list view tools */
void UI_view2d_listview_view_to_cell(float columnwidth,
                                     float rowheight,
                                     float startx,
                                     float starty,
                                     float viewx,
                                     float viewy,
                                     int *column,
                                     int *row);

/* coordinate conversion */
float UI_view2d_region_to_view_x(const struct View2D *v2d, float x);
float UI_view2d_region_to_view_y(const struct View2D *v2d, float y);
void UI_view2d_region_to_view(
    const struct View2D *v2d, float x, float y, float *r_view_x, float *r_view_y) ATTR_NONNULL();
void UI_view2d_region_to_view_rctf(const struct View2D *v2d,
                                   const struct rctf *rect_src,
                                   struct rctf *rect_dst) ATTR_NONNULL();

float UI_view2d_view_to_region_x(const struct View2D *v2d, float x);
float UI_view2d_view_to_region_y(const struct View2D *v2d, float y);
bool UI_view2d_view_to_region_clip(
    const struct View2D *v2d, float x, float y, int *r_region_x, int *r_region_y) ATTR_NONNULL();

void UI_view2d_view_to_region(
    const struct View2D *v2d, float x, float y, int *r_region_x, int *r_region_y) ATTR_NONNULL();
void UI_view2d_view_to_region_fl(const struct View2D *v2d,
                                 float x,
                                 float y,
                                 float *r_region_x,
                                 float *r_region_y) ATTR_NONNULL();
void UI_view2d_view_to_region_m4(const struct View2D *v2d, float matrix[4][4]) ATTR_NONNULL();
void UI_view2d_view_to_region_rcti(const struct View2D *v2d,
                                   const struct rctf *rect_src,
                                   struct rcti *rect_dst) ATTR_NONNULL();
bool UI_view2d_view_to_region_rcti_clip(const struct View2D *v2d,
                                        const struct rctf *rect_src,
                                        struct rcti *rect_dst) ATTR_NONNULL();

/* utilities */
struct View2D *UI_view2d_fromcontext(const struct bContext *C);
struct View2D *UI_view2d_fromcontext_rwin(const struct bContext *C);

void UI_view2d_scroller_size_get(const struct View2D *v2d, float *r_x, float *r_y);
void UI_view2d_scale_get(const struct View2D *v2d, float *r_x, float *r_y);
float UI_view2d_scale_get_x(const struct View2D *v2d);
float UI_view2d_scale_get_y(const struct View2D *v2d);
void UI_view2d_scale_get_inverse(const struct View2D *v2d, float *r_x, float *r_y);

void UI_view2d_center_get(const struct View2D *v2d, float *r_x, float *r_y);
void UI_view2d_center_set(struct View2D *v2d, float x, float y);

void UI_view2d_offset(struct View2D *v2d, float xfac, float yfac);

char UI_view2d_mouse_in_scrollers_ex(const struct ARegion *region,
                                     const struct View2D *v2d,
                                     const int xy[2],
                                     int *r_scroll) ATTR_NONNULL(1, 2, 3, 4);
char UI_view2d_mouse_in_scrollers(const struct ARegion *region,
                                  const struct View2D *v2d,
                                  const int xy[2]) ATTR_NONNULL(1, 2, 3);
char UI_view2d_rect_in_scrollers_ex(const struct ARegion *region,
                                    const struct View2D *v2d,
                                    const struct rcti *rect,
                                    int *r_scroll) ATTR_NONNULL(1, 2, 3);
char UI_view2d_rect_in_scrollers(const struct ARegion *region,
                                 const struct View2D *v2d,
                                 const struct rcti *rect) ATTR_NONNULL(1, 2, 3);

/* cached text drawing in v2d, to allow pixel-aligned draw as post process */
void UI_view2d_text_cache_add(struct View2D *v2d,
                              float x,
                              float y,
                              const char *str,
                              size_t str_len,
                              const unsigned char col[4]);
void UI_view2d_text_cache_add_rectf(struct View2D *v2d,
                                    const struct rctf *rect_view,
                                    const char *str,
                                    size_t str_len,
                                    const unsigned char col[4]);
void UI_view2d_text_cache_draw(struct ARegion *region);

/* operators */
void ED_operatortypes_view2d(void);
void ED_keymap_view2d(struct wmKeyConfig *keyconf);

void UI_view2d_smooth_view(struct bContext *C,
                           struct ARegion *region,
                           const struct rctf *cur,
                           const int smooth_viewtx);

#define UI_MARKER_MARGIN_Y (42 * UI_DPI_FAC)
#define UI_TIME_SCRUB_MARGIN_Y (23 * UI_DPI_FAC)

/* Gizmo Types */

/* view2d_gizmo_navigate.c */
/* Caller passes in own idname. */
void VIEW2D_GGT_navigate_impl(struct wmGizmoGroupType *gzgt, const char *idname);

/* Edge pan */

/**
 * Custom-data for view panning operators.
 */
typedef struct View2DEdgePanData {
  /** Screen where view pan was initiated. */
  struct bScreen *screen;
  /** Area where view pan was initiated. */
  struct ScrArea *area;
  /** Region where view pan was initiated. */
  struct ARegion *region;
  /** View2d we're operating in. */
  struct View2D *v2d;

  /** Inside distance in UI units from the edge of the region within which to start panning. */
  float inside_pad;
  /** Outside distance in UI units from the edge of the region at which to stop panning. */
  float outside_pad;
  /**
   * Width of the zone in UI units where speed increases with distance from the edge.
   * At the end of this zone max speed is reached.
   */
  float speed_ramp;
  /** Maximum speed in UI units per second. */
  float max_speed;
  /** Delay in seconds before maximum speed is reached. */
  float delay;
  /** Influence factor for view zoom:
   *    0 = Constant speed in UI units
   *    1 = Constant speed in view space, UI speed slows down when zooming out
   */
  float zoom_influence;

  /** Initial view rect. */
  rctf initial_rect;

  /** Amount to move view relative to zoom. */
  float facx, facy;

  /* Timers. */
  double edge_pan_last_time;
  double edge_pan_start_time_x, edge_pan_start_time_y;
} View2DEdgePanData;

bool UI_view2d_edge_pan_poll(struct bContext *C);

void UI_view2d_edge_pan_init(struct bContext *C,
                             struct View2DEdgePanData *vpd,
                             float inside_pad,
                             float outside_pad,
                             float speed_ramp,
                             float max_speed,
                             float delay,
                             float zoom_influence);

void UI_view2d_edge_pan_reset(struct View2DEdgePanData *vpd);

/* Apply transform to view (i.e. adjust 'cur' rect). */
void UI_view2d_edge_pan_apply(struct bContext *C, struct View2DEdgePanData *vpd, const int xy[2])
    ATTR_NONNULL(1, 2, 3);

/* Apply transform to view using mouse events. */
void UI_view2d_edge_pan_apply_event(struct bContext *C,
                                    struct View2DEdgePanData *vpd,
                                    const struct wmEvent *event);

void UI_view2d_edge_pan_cancel(struct bContext *C, struct View2DEdgePanData *vpd);

void UI_view2d_edge_pan_operator_properties(struct wmOperatorType *ot);

void UI_view2d_edge_pan_operator_properties_ex(struct wmOperatorType *ot,
                                               float inside_pad,
                                               float outside_pad,
                                               float speed_ramp,
                                               float max_speed,
                                               float delay,
                                               float zoom_influence);

/* Initialize panning data with operator settings. */
void UI_view2d_edge_pan_operator_init(struct bContext *C,
                                      struct View2DEdgePanData *vpd,
                                      struct wmOperator *op);

#ifdef __cplusplus
}
#endif
