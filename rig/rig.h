#ifndef _RIG_H_
#define _RIG_H_

#include <cogl/cogl.h>
#include <cogl-pango/cogl-pango.h>

#include <glib.h>

#ifndef __ANDROID__
#define USE_SDL
//#define USE_GLIB
#endif

typedef struct _RigContext RigContext;

#include "rig-global.h"
#include "rig-type.h"
#include "rig-object.h"
#include "rig-property.h"
#include "rig-interfaces.h"
#include "rig-shell.h"
#include "rig-bitmask.h"
#include "rig-stack.h"
#include "rig-timeline.h"
#include "rig-display-list.h"
#include "rig-arcball.h"
#include "rig-util.h"

/* entity/components system */
#include "rig-entity.h"
#include "rig-components.h"

/* PRIVATE */
void
_rig_init (void);

typedef struct _RigPaintableProps
{
  int padding;
} RigPaintableProps;

typedef struct _RigPaintContext
{
  RigCamera *camera;
} RigPaintContext;

#define RIG_PAINT_CONTEXT(X) ((RigPaintContext *)X)

typedef struct _RigPaintableVtable
{
  void (*paint) (RigObject *object, RigPaintContext *paint_ctx);
} RigPaintableVTable;

void
rig_paintable_init (RigObject *object);

void
rig_paintable_paint (RigObject *object, RigPaintContext *paint_ctx);

/* TODO Make internals private */
struct _RigContext
{
  RigObjectProps _parent;
  int ref_count;

  RigShell *shell;

  CoglContext *cogl_context;

  GHashTable *texture_cache;

  CoglIndices *nine_slice_indices;

  CoglPangoFontMap *pango_font_map;
  PangoContext *pango_context;
  PangoFontDescription *pango_font_desc;

  RigPropertyContext property_ctx;

  GSList *timelines;
};
#define RIG_CONTEXT(X) ((RigContext *)X)

extern RigType rig_context_type;

RigContext *
rig_context_new (RigShell *shell /* optional */);

void
rig_context_init (RigContext *context);

CoglTexture *
rig_load_texture (RigContext *ctx, const char *filename, GError **error);

typedef void (*RigCameraPaintCallback) (RigCamera *camera, void *user_data);

RigCamera *
rig_camera_new (RigContext *ctx, CoglFramebuffer *framebuffer);

CoglFramebuffer *
rig_camera_get_framebuffer (RigCamera *camera);

void
rig_camera_set_viewport (RigCamera *camera,
                         float x,
                         float y,
                         float width,
                         float height);

const float *
rig_camera_get_viewport (RigCamera *camera);

void
rig_camera_set_projection (RigCamera *camera,
                           const CoglMatrix *projection);

const CoglMatrix *
rig_camera_get_projection (RigCamera *camera);

const CoglMatrix *
rig_camera_get_inverse_projection (RigCamera *camera);

void
rig_camera_set_view_transform (RigCamera *camera,
                               const CoglMatrix *view);

const CoglMatrix *
rig_camera_get_view_transform (RigCamera *camera);

void
rig_camera_set_input_transform (RigCamera *camera,
                                const CoglMatrix *input_transform);

void
rig_camera_flush (RigCamera *camera);

void
rig_camera_end_frame (RigCamera *camera);

void
rig_camera_add_input_region (RigCamera *camera,
                             RigInputRegion *region);

void
rig_camera_remove_input_region (RigCamera *camera,
                                RigInputRegion *region);

CoglBool
rig_camera_transform_window_coordinate (RigCamera *camera,
                                        float *x,
                                        float *y);

void
rig_camera_unproject_coord (RigCamera *camera,
                            const CoglMatrix *modelview,
                            const CoglMatrix *inverse_modelview,
                            float object_coord_z,
                            float *x,
                            float *y);

#if 0
/* PRIVATE */
RigInputEventStatus
_rig_camera_input_callback_wrapper (RigCameraInputCallbackState *state,
                                    RigInputEvent *event);

void
rig_camera_add_input_callback (RigCamera *camera,
                               RigInputCallback callback,
                               void *user_data);
#endif

typedef struct _RigGraph RigGraph;
#define RIG_GRAPH(X) ((RigGraph *)X)

extern RigType rig_graph_type;

RigGraph *
rig_graph_new (RigContext *ctx, ...);


RigTransform *
rig_transform_new (RigContext *ctx, ...) G_GNUC_NULL_TERMINATED;

void
rig_transform_translate (RigTransform *transform,
                         float x,
                         float y,
                         float z);

void
rig_transform_quaternion_rotate (RigTransform *transform,
                                 const CoglQuaternion *quaternion);

void
rig_transform_scale (RigTransform *transform,
                     float x,
                     float y,
                     float z);

void
rig_transform_init_identity (RigTransform *transform);

const CoglMatrix *
rig_transform_get_matrix (RigTransform *transform);

typedef struct _RigNineSlice RigNineSlice;
#define RIG_NINE_SLICE(X) ((RigNineSlice *)X)

extern RigType rig_nine_slice_type;

RigNineSlice *
rig_nine_slice_new (RigContext *ctx,
                    CoglTexture *texture,
                    float top,
                    float right,
                    float bottom,
                    float left,
                    float width,
                    float height);

typedef struct _RigSimpleWidgetProps
{
  RigDisplayList display_list;
} RigSimpleWidgetProps;

typedef struct _RigSimpleWidgetVTable
{
  void (*set_camera) (RigObject *widget,
                      RigCamera *camera);
} RigSimpleWidgetVTable;

void
rig_simple_widget_graphable_parent_changed (RigObject *self,
                                            RigObject *old_parent,
                                            RigObject *new_parent);

/* Use for widget that can't have children */
void
rig_simple_widget_graphable_child_removed_warn (RigObject *self,
                                                RigObject *child);

void
rig_simple_widget_graphable_child_added_warn (RigObject *self,
                                              RigObject *child);

typedef struct _RigRectangle RigRectangle;
#define RIG_RECTANGLE(X) ((RigRectangle *)X)

extern RigType rig_rectangle_type;

RigRectangle *
rig_rectangle_new4f (RigContext *ctx,
                     float width,
                     float height,
                     float red,
                     float green,
                     float blue,
                     float alpha);

void
rig_rectangle_set_width (RigRectangle *rectangle, float width);

void
rig_rectangle_set_height (RigRectangle *rectangle, float height);

typedef struct _RigUIViewport RigUIViewport;
#define RIG_UI_VIEWPORT(X) ((RigUIViewport *)X)

extern RigType rig_ui_viewport_type;

RigUIViewport *
rig_ui_viewport_new (RigContext *ctx,
                     float width,
                     float height,
                     ...);

void
rig_ui_viewport_set_width (RigUIViewport *ui_viewport, float width);

void
rig_ui_viewport_set_height (RigUIViewport *ui_viewport, float height);

void
rig_ui_viewport_set_doc_x (RigUIViewport *ui_viewport, float doc_x);

void
rig_ui_viewport_set_doc_y (RigUIViewport *ui_viewport, float doc_y);

void
rig_ui_viewport_set_doc_scale_x (RigUIViewport *ui_viewport, float doc_scale_x);

void
rig_ui_viewport_set_doc_scale_y (RigUIViewport *ui_viewport, float doc_scale_y);

float
rig_ui_viewport_get_width (RigUIViewport *ui_viewport);

float
rig_ui_viewport_get_height (RigUIViewport *ui_viewport);

float
rig_ui_viewport_get_doc_x (RigUIViewport *ui_viewport);

float
rig_ui_viewport_get_doc_y (RigUIViewport *ui_viewport);

float
rig_ui_viewport_get_doc_scale_x (RigUIViewport *ui_viewport);

float
rig_ui_viewport_get_doc_scale_y (RigUIViewport *ui_viewport);

const CoglMatrix *
rig_ui_viewport_get_doc_matrix (RigUIViewport *ui_viewport);

typedef struct _RigButton RigButton;
#define RIG_BUTTON(X) ((RigButton *)X)

extern RigType rig_button_type;

RigButton *
rig_button_new (RigContext *ctx,
                const char *label);

typedef void (*RigButtonClickCallback) (RigButton *button, void *user_data);

void
rig_button_set_on_click_callback (RigButton *button,
                                  RigButtonClickCallback callback,
                                  void *user_data);

CoglTexture *
_rig_load_texture (RigContext *ctx, const char *filename, GError **error);

#endif /* _RIG_H_ */
