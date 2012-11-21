#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>

#include <cogl/cogl.h>

#include <rut.h>

#include "rig-data.h"
#include "rig-transition.h"
#include "rig-load-save.h"
#include "rig-undo-journal.h"
#include "rig-renderer.h"

//#define DEVICE_WIDTH 480.0
//#define DEVICE_HEIGHT 800.0
#define DEVICE_WIDTH 720.0
#define DEVICE_HEIGHT 1280.0

/*
 * Note: The size and padding for this circle texture have been carefully
 * chosen so it has a power of two size and we have enough padding to scale
 * down the circle to a size of 2 pixels and still have a 1 texel transparent
 * border which we rely on for anti-aliasing.
 */
#define CIRCLE_TEX_RADIUS 16
#define CIRCLE_TEX_PADDING 16

#define N_CUBES 5

static RutPropertySpec rut_data_property_specs[] = {
  {
    .name = "width",
    .flags = RUT_PROPERTY_FLAG_READABLE,
    .type = RUT_PROPERTY_TYPE_FLOAT,
    .data_offset = offsetof (RigData, width)
  },
  {
    .name = "height",
    .flags = RUT_PROPERTY_FLAG_READABLE,
    .type = RUT_PROPERTY_TYPE_FLOAT,
    .data_offset = offsetof (RigData, height)
  },
  {
    .name = "device_width",
    .flags = RUT_PROPERTY_FLAG_READABLE,
    .type = RUT_PROPERTY_TYPE_FLOAT,
    .data_offset = offsetof (RigData, device_width)
  },
  {
    .name = "device_height",
    .flags = RUT_PROPERTY_FLAG_READABLE,
    .type = RUT_PROPERTY_TYPE_FLOAT,
    .data_offset = offsetof (RigData, device_height)
  },
  { 0 }
};

static void
rig_load_asset_list (RigData *data);

#ifndef __ANDROID__

#ifdef RIG_EDITOR_ENABLED
CoglBool _rig_in_device_mode = FALSE;
#endif

static char **_rig_handset_remaining_args = NULL;
static const char *_rig_ui_filename = NULL;

static const GOptionEntry rut_handset_entries[] =
{
#ifdef RIG_EDITOR_ENABLED
  { "device-mode", 'd', 0, 0,
    &_rig_in_device_mode, "Run in Device Mode" },
#endif
  { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY,
    &_rig_handset_remaining_args, "Project" },
  { 0 }
};

#endif /* __ANDROID__ */

static RutTraverseVisitFlags
scenegraph_pre_paint_cb (RutObject *object,
                         int depth,
                         void *user_data)
{
  RutPaintContext *rut_paint_ctx = user_data;
  RutCamera *camera = rut_paint_ctx->camera;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (camera);

#if 0
  if (rut_object_get_type (object) == &rut_camera_type)
    {
      g_print ("%*sCamera = %p\n", depth, "", object);
      rut_camera_flush (RUT_CAMERA (object));
      return RUT_TRAVERSE_VISIT_CONTINUE;
    }
  else
#endif

  if (rut_object_get_type (object) == &rut_ui_viewport_type)
    {
      RutUIViewport *ui_viewport = RUT_UI_VIEWPORT (object);
#if 0
      g_print ("%*sPushing clip = %f %f\n",
               depth, "",
               rut_ui_viewport_get_width (ui_viewport),
               rut_ui_viewport_get_height (ui_viewport));
#endif
      cogl_framebuffer_push_rectangle_clip (fb,
                                            0, 0,
                                            rut_ui_viewport_get_width (ui_viewport),
                                            rut_ui_viewport_get_height (ui_viewport));
    }

  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      //g_print ("%*sTransformable = %p\n", depth, "", object);
      const CoglMatrix *matrix = rut_transformable_get_matrix (object);
      //cogl_debug_matrix_print (matrix);
      cogl_framebuffer_push_matrix (fb);
      cogl_framebuffer_transform (fb, matrix);
    }

  if (rut_object_is (object, RUT_INTERFACE_ID_PAINTABLE))
    {
      RutPaintableVTable *vtable =
        rut_object_get_vtable (object, RUT_INTERFACE_ID_PAINTABLE);
      vtable->paint (object, rut_paint_ctx);
    }

  /* XXX:
   * How can we maintain state between the pre and post stages?  Is it
   * ok to just "sub-class" the paint context and maintain a stack of
   * state that needs to be shared with the post paint code.
   */

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static RutTraverseVisitFlags
scenegraph_post_paint_cb (RutObject *object,
                          int depth,
                          void *user_data)
{
  RutPaintContext *rut_paint_ctx = user_data;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (rut_paint_ctx->camera);

#if 0
  if (rut_object_get_type (object) == &rut_camera_type)
    {
      rut_camera_end_frame (RUT_CAMERA (object));
      return RUT_TRAVERSE_VISIT_CONTINUE;
    }
  else
#endif

  if (rut_object_get_type (object) == &rut_ui_viewport_type)
    {
      cogl_framebuffer_pop_clip (fb);
    }

  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      cogl_framebuffer_pop_matrix (fb);
    }

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static CoglBool
paint (RutShell *shell, void *user_data)
{
  RigData *data = user_data;
  CoglFramebuffer *fb = rut_camera_get_framebuffer (data->camera);
  RigPaintContext paint_ctx;
  RutPaintContext *rut_paint_ctx = &paint_ctx._parent;

  cogl_framebuffer_clear4f (fb,
                            COGL_BUFFER_BIT_COLOR|COGL_BUFFER_BIT_DEPTH,
                            0.22, 0.22, 0.22, 1);

  paint_ctx.data = data;
  paint_ctx.pass = RIG_PASS_COLOR_BLENDED;
  rut_paint_ctx->camera = data->camera;

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      rut_camera_flush (data->camera);
      rut_paint_graph_with_layers (data->root,
                                   scenegraph_pre_paint_cb,
                                   scenegraph_post_paint_cb,
                                   rut_paint_ctx);
      /* FIXME: this should be moved to the end of this function but we
       * currently get warnings about unbalanced _flush()/_end_frame()
       * pairs. */
      rut_camera_end_frame (data->camera);
    }
#endif

  paint_ctx.pass = RIG_PASS_SHADOW;
  rig_camera_update_view (data, data->light, TRUE);
  rig_paint_camera_entity (data->light, &paint_ctx);

  rig_camera_update_view (data, data->editor_camera, FALSE);

  if (data->enable_dof)
    {
      RutCamera *camera_component =
        rut_entity_get_component (data->editor_camera,
                                  RUT_COMPONENT_TYPE_CAMERA);
      const float *viewport = rut_camera_get_viewport (camera_component);
      int width = viewport[2];
      int height = viewport[3];
      int save_viewport_x = viewport[0];
      int save_viewport_y = viewport[1];
      CoglFramebuffer *pass_fb;

      rut_dof_effect_set_framebuffer_size (data->dof, width, height);

      pass_fb = rut_dof_effect_get_depth_pass_fb (data->dof);
      rut_camera_set_framebuffer (camera_component, pass_fb);
      rut_camera_set_viewport (camera_component, 0, 0, width, height);

      rut_camera_flush (camera_component);
      cogl_framebuffer_clear4f (pass_fb,
                                COGL_BUFFER_BIT_COLOR|COGL_BUFFER_BIT_DEPTH,
                                1, 1, 1, 1);
      rut_camera_end_frame (camera_component);

      paint_ctx.pass = RIG_PASS_DOF_DEPTH;
      rig_paint_camera_entity (data->editor_camera, &paint_ctx);

      pass_fb = rut_dof_effect_get_color_pass_fb (data->dof);
      rut_camera_set_framebuffer (camera_component, pass_fb);

      rut_camera_flush (camera_component);
      cogl_framebuffer_clear4f (pass_fb,
                                COGL_BUFFER_BIT_COLOR|COGL_BUFFER_BIT_DEPTH,
                                0.22, 0.22, 0.22, 1);
      rut_camera_end_frame (camera_component);

      paint_ctx.pass = RIG_PASS_COLOR_UNBLENDED;
      rig_paint_camera_entity (data->editor_camera, &paint_ctx);

      paint_ctx.pass = RIG_PASS_COLOR_BLENDED;
      rig_paint_camera_entity (data->editor_camera, &paint_ctx);

      rut_camera_set_framebuffer (camera_component, fb);
      rut_camera_set_viewport (camera_component,
                               save_viewport_x,
                               save_viewport_y,
                               width, height);

      rut_camera_flush (data->camera);
      rut_dof_effect_draw_rectangle (data->dof,
                                     fb,
                                     data->main_x,
                                     data->main_y,
                                     data->main_x + data->main_width,
                                     data->main_y + data->main_height);
      rut_camera_end_frame (data->camera);
    }
  else
    {
      paint_ctx.pass = RIG_PASS_COLOR_UNBLENDED;
      rut_paint_ctx->camera = data->camera;
      rig_paint_camera_entity (data->editor_camera, &paint_ctx);

      paint_ctx.pass = RIG_PASS_COLOR_BLENDED;
      rut_paint_ctx->camera = data->camera;
      rig_paint_camera_entity (data->editor_camera, &paint_ctx);
    }

  rut_camera_flush (data->editor_camera_component);

  /* Use this to visualize the depth-of-field alpha buffer... */
#if 0
  CoglPipeline *pipeline = cogl_pipeline_new (data->ctx->cogl_context);
  cogl_pipeline_set_layer_texture (pipeline, 0, data->dof.depth_pass);
  cogl_pipeline_set_blend (pipeline, "RGBA=ADD(SRC_COLOR, 0)", NULL);
  cogl_framebuffer_draw_rectangle (fb,
                                   pipeline,
                                   0, 0,
                                   200, 200);
#endif

  /* Use this to visualize the shadow_map */
#if 0
  CoglPipeline *pipeline = cogl_pipeline_new (data->ctx->cogl_context);
  cogl_pipeline_set_layer_texture (pipeline, 0, data->shadow_map);
  //cogl_pipeline_set_layer_texture (pipeline, 0, data->shadow_color);
  cogl_pipeline_set_blend (pipeline, "RGBA=ADD(SRC_COLOR, 0)", NULL);
  cogl_framebuffer_draw_rectangle (fb,
                                   pipeline,
                                   0, 0,
                                   200, 200);
#endif

  if (data->debug_pick_ray && data->picking_ray)
    {
      cogl_framebuffer_draw_primitive (fb,
                                       data->picking_ray_color,
                                       data->picking_ray);
    }

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode && !data->play_mode)
    {
      rut_util_draw_jittered_primitive3f (fb, data->grid_prim, 0.5, 0.5, 0.5);

      if (data->selected_entity)
        {
          rut_tool_update (data->tool, data->selected_entity);
          rut_tool_draw (data->tool, fb);
        }
    }
#endif /* RIG_EDITOR_ENABLED */

  rut_camera_end_frame (data->editor_camera_component);

  cogl_onscreen_swap_buffers (COGL_ONSCREEN (fb));

  return FALSE;
}

void
rig_reload_inspector_property (RigData *data,
                               RutProperty *property)
{
  if (data->inspector)
    {
      RigTransitionPropData *prop_data;
      CoglBool animated;
      GList *l;

      prop_data =
        rig_transition_find_prop_data_for_property (data->selected_transition,
                                                    property);

      animated = prop_data && prop_data->animated;

      for (l = data->all_inspectors; l; l = l->next)
        {
          rut_inspector_reload_property (l->data, property);
          rut_inspector_set_property_animated (l->data, property, animated);
        }
    }
}

static void
reload_animated_inspector_properties_cb (RigTransitionPropData *prop_data,
                                         void *user_data)
{
  RigData *data = user_data;

  if (prop_data->animated)
    rig_reload_inspector_property (data, prop_data->property);
}

static void
reload_animated_inspector_properties (RigData *data)
{
  if (data->inspector && data->selected_transition)
    rig_transition_foreach_property (data->selected_transition,
                                     reload_animated_inspector_properties_cb,
                                     data);
}

static void
update_transition_progress_cb (RutProperty *target_property,
                               RutProperty *source_property,
                               void *user_data)
{
  RigData *data = user_data;
  double progress = rut_timeline_get_progress (data->timeline);
  RigTransition *transition = target_property->object;

  rig_transition_set_progress (transition, progress);
  reload_animated_inspector_properties (data);
}

RigTransition *
rig_create_transition (RigData *data,
                       uint32_t id)
{
  RigTransition *transition = rig_transition_new (data->ctx, id);

  /* FIXME: this should probably only update the progress for the
   * current transition */
  rut_property_set_binding (&transition->props[RUT_TRANSITION_PROP_PROGRESS],
                            update_transition_progress_cb,
                            data,
                            data->timeline_elapsed,
                            NULL);

  return transition;
}

static void
unproject_window_coord (RutCamera *camera,
                        const CoglMatrix *modelview,
                        const CoglMatrix *inverse_modelview,
                        float object_coord_z,
                        float *x,
                        float *y)
{
  const CoglMatrix *projection = rut_camera_get_projection (camera);
  const CoglMatrix *inverse_projection =
    rut_camera_get_inverse_projection (camera);
  //float z;
  float ndc_x, ndc_y, ndc_z, ndc_w;
  float eye_x, eye_y, eye_z, eye_w;
  const float *viewport = rut_camera_get_viewport (camera);

  /* Convert object coord z into NDC z */
  {
    float tmp_x, tmp_y, tmp_z;
    const CoglMatrix *m = modelview;
    float z, w;

    tmp_x = m->xz * object_coord_z + m->xw;
    tmp_y = m->yz * object_coord_z + m->yw;
    tmp_z = m->zz * object_coord_z + m->zw;

    m = projection;
    z = m->zx * tmp_x + m->zy * tmp_y + m->zz * tmp_z + m->zw;
    w = m->wx * tmp_x + m->wy * tmp_y + m->wz * tmp_z + m->ww;

    ndc_z = z / w;
  }

  /* Undo the Viewport transform, putting us in Normalized Device Coords */
  ndc_x = (*x - viewport[0]) * 2.0f / viewport[2] - 1.0f;
  ndc_y = ((viewport[3] - 1 + viewport[1] - *y) * 2.0f / viewport[3] - 1.0f);

  /* Undo the Projection, putting us in Eye Coords. */
  ndc_w = 1;
  cogl_matrix_transform_point (inverse_projection,
                               &ndc_x, &ndc_y, &ndc_z, &ndc_w);
  eye_x = ndc_x / ndc_w;
  eye_y = ndc_y / ndc_w;
  eye_z = ndc_z / ndc_w;
  eye_w = 1;

  /* Undo the Modelview transform, putting us in Object Coords */
  cogl_matrix_transform_point (inverse_modelview,
                               &eye_x,
                               &eye_y,
                               &eye_z,
                               &eye_w);

  *x = eye_x;
  *y = eye_y;
  //*z = eye_z;
}

typedef void (*EntityTranslateCallback)(RutEntity *entity,
                                        float start[3],
                                        float rel[3],
                                        void *user_data);

typedef void (*EntityTranslateDoneCallback)(RutEntity *entity,
                                            CoglBool moved,
                                            float start[3],
                                            float rel[3],
                                            void *user_data);

typedef struct _EntityTranslateGrabClosure
{
  RigData *data;

  /* pointer position at start of grab */
  float grab_x;
  float grab_y;

  /* entity position at start of grab */
  float entity_grab_pos[3];
  RutEntity *entity;

  /* set as soon as a move event is encountered so that we can detect
   * situations where a grab is started but nothing actually moves */
  CoglBool moved;

  float x_vec[3];
  float y_vec[3];

  EntityTranslateCallback entity_translate_cb;
  EntityTranslateDoneCallback entity_translate_done_cb;
  void *user_data;
} EntityTranslateGrabClosure;

static RutInputEventStatus
entity_translate_grab_input_cb (RutInputEvent *event,
                                void *user_data)

{
  EntityTranslateGrabClosure *closure = user_data;
  RutEntity *entity = closure->entity;
  RigData *data = closure->data;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      float x = rut_motion_event_get_x (event);
      float y = rut_motion_event_get_y (event);
      float move_x, move_y;
      float rel[3];
      float *x_vec = closure->x_vec;
      float *y_vec = closure->y_vec;

      move_x = x - closure->grab_x;
      move_y = y - closure->grab_y;

      rel[0] = x_vec[0] * move_x;
      rel[1] = x_vec[1] * move_x;
      rel[2] = x_vec[2] * move_x;

      rel[0] += y_vec[0] * move_y;
      rel[1] += y_vec[1] * move_y;
      rel[2] += y_vec[2] * move_y;

      if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_UP)
        {
          if (closure->entity_translate_done_cb)
            closure->entity_translate_done_cb (entity,
                                               closure->moved,
                                               closure->entity_grab_pos,
                                               rel,
                                               closure->user_data);

          rut_shell_ungrab_input (data->ctx->shell,
                                  entity_translate_grab_input_cb,
                                  user_data);

          g_slice_free (EntityTranslateGrabClosure, user_data);

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_MOVE)
        {
          closure->moved = TRUE;

          closure->entity_translate_cb (entity,
                                        closure->entity_grab_pos,
                                        rel,
                                        closure->user_data);

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static void
inspector_property_changed_cb (RutProperty *target_property,
                               RutProperty *source_property,
                               void *user_data)
{
  RigData *data = user_data;
  RutBoxed new_value;

  rut_property_box (source_property, &new_value);

  rig_undo_journal_set_property_and_log (data->undo_journal,
                                         TRUE, /* mergable */
                                         &new_value,
                                         target_property);

  rut_boxed_destroy (&new_value);
}

static void
inspector_animated_changed_cb (RutProperty *property,
                               CoglBool value,
                               void *user_data)
{
  RigData *data = user_data;
  RigPath *path;

  /* If the property is being initially marked as animated and the
   * path is empty then for convenience we want to create a node for
   * the current time. We want this to be undone as a single action so
   * we'll represent the pair of actions in a subjournal */
  if (value &&
      (path = rig_transition_get_path_for_property (data->selected_transition,
                                                    property)) &&
      path->length == 0)
    {
      RigUndoJournal *subjournal = rig_undo_journal_new (data);
      RutBoxed property_value;

      rut_property_box (property, &property_value);

      rig_undo_journal_set_animated_and_log (subjournal,
                                             property,
                                             value);
      rig_undo_journal_set_property_and_log (subjournal,
                                             FALSE /* mergable */,
                                             &property_value,
                                             property);

      rig_undo_journal_log_subjournal (data->undo_journal, subjournal);

      rut_boxed_destroy (&property_value);
    }
  else
    rig_undo_journal_set_animated_and_log (data->undo_journal,
                                           property,
                                           value);
}

typedef struct
{
  RigData *data;
  RutInspector *inspector;
} InitAnimatedStateData;

static void
init_property_animated_state_cb (RutProperty *property,
                                 void *user_data)
{
  InitAnimatedStateData *data = user_data;

  if (property->spec->animatable)
    {
      RigTransitionPropData *prop_data;
      RigTransition *transition = data->data->selected_transition;

      prop_data =
        rig_transition_find_prop_data_for_property (transition, property);

      if (prop_data && prop_data->animated)
        rut_inspector_set_property_animated (data->inspector, property, TRUE);
    }
}

static RutInspector *
create_inspector (RigData *data,
                  void *object)
{
  RutInspector *inspector =
    rut_inspector_new (data->ctx,
                       object,
                       inspector_property_changed_cb,
                       inspector_animated_changed_cb,
                       data);

  if (rut_object_is (object, RUT_INTERFACE_ID_INTROSPECTABLE))
    {
      InitAnimatedStateData animated_data;

      animated_data.data = data;
      animated_data.inspector = inspector;

      rut_introspectable_foreach_property (object,
                                           init_property_animated_state_cb,
                                           &animated_data);
    }

  return inspector;
}

static void
add_component_inspector_cb (RutComponent *component,
                            void *user_data)
{
  RigData *data = user_data;
  RutInspector *inspector = create_inspector (data, component);

  rut_box_layout_add (data->inspector_box_layout, inspector);

  data->all_inspectors =
    g_list_prepend (data->all_inspectors, inspector);
}

static void
update_inspector (RigData *data)
{
  GList *l;

  for (l = data->all_inspectors; l; l = l->next)
    {
      rut_box_layout_remove (data->inspector_box_layout, l->data);
      rut_refable_unref (l->data);
    }

  data->inspector = NULL;
  g_list_free (data->all_inspectors);
  data->all_inspectors = NULL;

  if (data->selected_entity)
    {
      data->inspector = create_inspector (data, data->selected_entity);

      rut_box_layout_add (data->inspector_box_layout, data->inspector);
      data->all_inspectors =
        g_list_prepend (data->all_inspectors, data->inspector);

      rut_entity_foreach_component (data->selected_entity,
                                    add_component_inspector_cb,
                                    data);
    }
}

static CoglPrimitive *
create_line_primitive (float a[3], float b[3])
{
  CoglVertexP3 data[2];
  CoglAttributeBuffer *attribute_buffer;
  CoglAttribute *attributes[1];
  CoglPrimitive *primitive;

  data[0].x = a[0];
  data[0].y = a[1];
  data[0].z = a[2];
  data[1].x = b[0];
  data[1].y = b[1];
  data[1].z = b[2];

  attribute_buffer = cogl_attribute_buffer_new (rut_cogl_context,
                                                2 * sizeof (CoglVertexP3),
                                                data);

  attributes[0] = cogl_attribute_new (attribute_buffer,
                                      "cogl_position_in",
                                      sizeof (CoglVertexP3),
                                      offsetof (CoglVertexP3, x),
                                      3,
                                      COGL_ATTRIBUTE_TYPE_FLOAT);

  primitive = cogl_primitive_new_with_attributes (COGL_VERTICES_MODE_LINES,
                                                  2, attributes, 1);

  cogl_object_unref (attribute_buffer);
  cogl_object_unref (attributes[0]);

  return primitive;
}

static void
transform_ray (CoglMatrix *transform,
               bool        inverse_transform,
               float       ray_origin[3],
               float       ray_direction[3])
{
  CoglMatrix inverse, normal_matrix, *m;

  m = transform;
  if (inverse_transform)
    {
      cogl_matrix_get_inverse (transform, &inverse);
      m = &inverse;
    }

  cogl_matrix_transform_points (m,
                                3, /* num components for input */
                                sizeof (float) * 3, /* input stride */
                                ray_origin,
                                sizeof (float) * 3, /* output stride */
                                ray_origin,
                                1 /* n_points */);

  cogl_matrix_get_inverse (m, &normal_matrix);
  cogl_matrix_transpose (&normal_matrix);

  rut_util_transform_normal (&normal_matrix,
                             &ray_direction[0],
                             &ray_direction[1],
                             &ray_direction[2]);
}

static CoglPrimitive *
create_picking_ray (RigData            *data,
                    CoglFramebuffer *fb,
                    float            ray_position[3],
                    float            ray_direction[3],
                    float            length)
{
  CoglPrimitive *line;
  float points[6];

  points[0] = ray_position[0];
  points[1] = ray_position[1];
  points[2] = ray_position[2];
  points[3] = ray_position[0] + length * ray_direction[0];
  points[4] = ray_position[1] + length * ray_direction[1];
  points[5] = ray_position[2] + length * ray_direction[2];

  line = create_line_primitive (points, points + 3);

  return line;
}

typedef struct _PickContext
{
  RutCamera *camera;
  CoglFramebuffer *fb;
  float *ray_origin;
  float *ray_direction;
  RutEntity *selected_entity;
  float selected_distance;
  int selected_index;
} PickContext;

static RutTraverseVisitFlags
entitygraph_pre_pick_cb (RutObject *object,
                         int depth,
                         void *user_data)
{
  PickContext *pick_ctx = user_data;
  CoglFramebuffer *fb = pick_ctx->fb;

  /* XXX: It could be nice if Cogl exposed matrix stacks directly, but for now
   * we just take advantage of an arbitrary framebuffer matrix stack so that
   * we can avoid repeated accumulating the transform of ancestors when
   * traversing between scenegraph nodes that have common ancestors.
   */
  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      const CoglMatrix *matrix = rut_transformable_get_matrix (object);
      cogl_framebuffer_push_matrix (fb);
      cogl_framebuffer_transform (fb, matrix);
    }

  if (rut_object_get_type (object) == &rut_entity_type)
    {
      RutEntity *entity = object;
      RutComponent *geometry;
      RutMesh *mesh;
      int index;
      float distance;
      bool hit;
      float transformed_ray_origin[3];
      float transformed_ray_direction[3];
      CoglMatrix transform;

      if (!rut_entity_get_visible (entity))
        return RUT_TRAVERSE_VISIT_CONTINUE;

      geometry = rut_entity_get_component (entity, RUT_COMPONENT_TYPE_GEOMETRY);

      /* Get a model we can pick against */
      if (!(geometry &&
            rut_object_is (geometry, RUT_INTERFACE_ID_PICKABLE) &&
            (mesh = rut_pickable_get_mesh (geometry))))
        return RUT_TRAVERSE_VISIT_CONTINUE;

      /* transform the ray into the model space */
      memcpy (transformed_ray_origin,
              pick_ctx->ray_origin, 3 * sizeof (float));
      memcpy (transformed_ray_direction,
              pick_ctx->ray_direction, 3 * sizeof (float));

      cogl_framebuffer_get_modelview_matrix (fb, &transform);

      transform_ray (&transform,
                     TRUE, /* inverse of the transform */
                     transformed_ray_origin,
                     transformed_ray_direction);

      /* intersect the transformed ray with the model data */
      hit = rut_util_intersect_mesh (mesh,
                                     transformed_ray_origin,
                                     transformed_ray_direction,
                                     &index,
                                     &distance);

      if (hit)
        {
          const CoglMatrix *view = rut_camera_get_view_transform (pick_ctx->camera);
          float w = 1;

          /* to compare intersection distances we find the actual point of ray
           * intersection in model coordinates and transform that into eye
           * coordinates */

          transformed_ray_direction[0] *= distance;
          transformed_ray_direction[1] *= distance;
          transformed_ray_direction[2] *= distance;

          transformed_ray_direction[0] += transformed_ray_origin[0];
          transformed_ray_direction[1] += transformed_ray_origin[1];
          transformed_ray_direction[2] += transformed_ray_origin[2];

          cogl_matrix_transform_point (&transform,
                                       &transformed_ray_direction[0],
                                       &transformed_ray_direction[1],
                                       &transformed_ray_direction[2],
                                       &w);
          cogl_matrix_transform_point (view,
                                       &transformed_ray_direction[0],
                                       &transformed_ray_direction[1],
                                       &transformed_ray_direction[2],
                                       &w);
          distance = transformed_ray_direction[2];

          if (distance > pick_ctx->selected_distance)
            {
              pick_ctx->selected_entity = entity;
              pick_ctx->selected_distance = distance;
              pick_ctx->selected_index = index;
            }
        }
    }

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static RutTraverseVisitFlags
entitygraph_post_pick_cb (RutObject *object,
                          int depth,
                          void *user_data)
{
  if (rut_object_is (object, RUT_INTERFACE_ID_TRANSFORMABLE))
    {
      PickContext *pick_ctx = user_data;
      cogl_framebuffer_pop_matrix (pick_ctx->fb);
    }

  return RUT_TRAVERSE_VISIT_CONTINUE;
}

static RutEntity *
pick (RigData *data,
      RutCamera *camera,
      CoglFramebuffer *fb,
      float ray_origin[3],
      float ray_direction[3])
{
  PickContext pick_ctx;

  pick_ctx.camera = camera;
  pick_ctx.fb = fb;
  pick_ctx.selected_distance = -G_MAXFLOAT;
  pick_ctx.selected_entity = NULL;
  pick_ctx.ray_origin = ray_origin;
  pick_ctx.ray_direction = ray_direction;

  /* We are hijacking the framebuffer's matrix to track the graphable
   * transforms so we need to initialise it to a known state */
  cogl_framebuffer_identity_matrix (fb);

  rut_graphable_traverse (data->scene,
                          RUT_TRAVERSE_DEPTH_FIRST,
                          entitygraph_pre_pick_cb,
                          entitygraph_post_pick_cb,
                          &pick_ctx);

  if (pick_ctx.selected_entity)
    {
      g_message ("Hit entity, triangle #%d, distance %.2f",
                 pick_ctx.selected_index, pick_ctx.selected_distance);
    }

  return pick_ctx.selected_entity;
}

static void
update_camera_position (RigData *data)
{
  rut_entity_set_position (data->editor_camera_to_origin,
                           data->origin);

  rut_entity_set_translate (data->editor_camera_armature, 0, 0, data->editor_camera_z);

  rut_shell_queue_redraw (data->ctx->shell);
}

static void
print_quaternion (const CoglQuaternion *q,
                  const char *label)
{
  float angle = cogl_quaternion_get_rotation_angle (q);
  float axis[3];
  cogl_quaternion_get_rotation_axis (q, axis);
  g_print ("%s: [%f (%f, %f, %f)]\n", label, angle, axis[0], axis[1], axis[2]);
}

static CoglBool
translate_grab_entity (RigData *data,
                       RutCamera *camera,
                       RutEntity *entity,
                       float grab_x,
                       float grab_y,
                       EntityTranslateCallback translate_cb,
                       EntityTranslateDoneCallback done_cb,
                       void *user_data)
{
  EntityTranslateGrabClosure *closure =
    g_slice_new (EntityTranslateGrabClosure);
  RutEntity *parent = rut_graphable_get_parent (entity);
  CoglMatrix parent_transform;
  CoglMatrix inverse_transform;
  float origin[3] = {0, 0, 0};
  float unit_x[3] = {1, 0, 0};
  float unit_y[3] = {0, 1, 0};
  float x_vec[3];
  float y_vec[3];
  float entity_x, entity_y, entity_z;
  float w;

  if (!parent)
    return FALSE;

  rut_graphable_get_modelview (parent, camera, &parent_transform);

  if (!cogl_matrix_get_inverse (&parent_transform, &inverse_transform))
    {
      g_warning ("Failed to get inverse transform of entity");
      return FALSE;
    }

  /* Find the z of our selected entity in eye coordinates */
  entity_x = 0;
  entity_y = 0;
  entity_z = 0;
  w = 1;
  cogl_matrix_transform_point (&parent_transform,
                               &entity_x, &entity_y, &entity_z, &w);

  //g_print ("Entity origin in eye coords: %f %f %f\n", entity_x, entity_y, entity_z);

  /* Convert unit x and y vectors in screen coordinate
   * into points in eye coordinates with the same z depth
   * as our selected entity */

  unproject_window_coord (camera,
                          &data->identity, &data->identity,
                          entity_z, &origin[0], &origin[1]);
  origin[2] = entity_z;
  //g_print ("eye origin: %f %f %f\n", origin[0], origin[1], origin[2]);

  unproject_window_coord (camera,
                          &data->identity, &data->identity,
                          entity_z, &unit_x[0], &unit_x[1]);
  unit_x[2] = entity_z;
  //g_print ("eye unit_x: %f %f %f\n", unit_x[0], unit_x[1], unit_x[2]);

  unproject_window_coord (camera,
                          &data->identity, &data->identity,
                          entity_z, &unit_y[0], &unit_y[1]);
  unit_y[2] = entity_z;
  //g_print ("eye unit_y: %f %f %f\n", unit_y[0], unit_y[1], unit_y[2]);


  /* Transform our points from eye coordinates into entity
   * coordinates and convert into input mapping vectors */

  w = 1;
  cogl_matrix_transform_point (&inverse_transform,
                               &origin[0], &origin[1], &origin[2], &w);
  w = 1;
  cogl_matrix_transform_point (&inverse_transform,
                               &unit_x[0], &unit_x[1], &unit_x[2], &w);
  w = 1;
  cogl_matrix_transform_point (&inverse_transform,
                               &unit_y[0], &unit_y[1], &unit_y[2], &w);


  x_vec[0] = unit_x[0] - origin[0];
  x_vec[1] = unit_x[1] - origin[1];
  x_vec[2] = unit_x[2] - origin[2];

  //g_print (" =========================== Entity coords: x_vec = %f, %f, %f\n",
  //         x_vec[0], x_vec[1], x_vec[2]);

  y_vec[0] = unit_y[0] - origin[0];
  y_vec[1] = unit_y[1] - origin[1];
  y_vec[2] = unit_y[2] - origin[2];

  //g_print (" =========================== Entity coords: y_vec = %f, %f, %f\n",
  //         y_vec[0], y_vec[1], y_vec[2]);

  closure->data = data;
  closure->grab_x = grab_x;
  closure->grab_y = grab_y;

  memcpy (closure->entity_grab_pos,
          rut_entity_get_position (entity),
          sizeof (float) * 3);

  closure->entity = entity;
  closure->entity_translate_cb = translate_cb;
  closure->entity_translate_done_cb = done_cb;
  closure->moved = FALSE;
  closure->user_data = user_data;

  memcpy (closure->x_vec, x_vec, sizeof (float) * 3);
  memcpy (closure->y_vec, y_vec, sizeof (float) * 3);

  rut_shell_grab_input (data->ctx->shell,
                        camera,
                        entity_translate_grab_input_cb,
                        closure);

  return TRUE;
}

static void
reload_position_inspector (RigData *data,
                           RutEntity *entity)
{
  if (data->inspector)
    {
      RutProperty *property =
        rut_introspectable_lookup_property (entity, "position");

      rut_inspector_reload_property (data->inspector, property);
    }
}

static void
entity_translate_done_cb (RutEntity *entity,
                          CoglBool moved,
                          float start[3],
                          float rel[3],
                          void *user_data)
{
  RigData *data = user_data;

  /* If the entity hasn't actually moved then we'll ignore it. It that
   * case the user is presumably just trying to select and entity we
   * don't want it to modify the transition */
  if (moved)
    {
      rig_undo_journal_move_and_log (data->undo_journal,
                                     FALSE, /* mergable */
                                     entity,
                                     start[0] + rel[0],
                                     start[1] + rel[1],
                                     start[2] + rel[2]);

      reload_position_inspector (data, entity);

      rut_shell_queue_redraw (data->ctx->shell);
    }
}

static void
entity_translate_cb (RutEntity *entity,
                     float start[3],
                     float rel[3],
                     void *user_data)
{
  RigData *data = user_data;

  rut_entity_set_translate (entity,
                            start[0] + rel[0],
                            start[1] + rel[1],
                            start[2] + rel[2]);

  reload_position_inspector (data, entity);

  rut_shell_queue_redraw (data->ctx->shell);
}

static void
tool_rotation_event_cb (RutTool *tool,
                        RutToolRotationEventType type,
                        const CoglQuaternion *rotation,
                        void *user_data)
{
  RigData *data = user_data;

  g_return_if_fail (data->selected_entity);

  switch (type)
    {
    case RUT_TOOL_ROTATION_DRAG:
      rut_entity_set_rotation (data->selected_entity, rotation);
      rut_shell_queue_redraw (data->shell);
      break;

    case RUT_TOOL_ROTATION_RELEASE:
      {
        RutProperty *rotation_prop =
          rut_introspectable_lookup_property (data->selected_entity,
                                              "rotation");
        RutBoxed value;

        value.type = RUT_PROPERTY_TYPE_QUATERNION;
        value.d.quaternion_val = *rotation;

        rig_undo_journal_set_property_and_log (data->undo_journal,
                                               FALSE /* mergable */,
                                               &value,
                                               rotation_prop);
      }
      break;
    }
}

static void
scene_translate_cb (RutEntity *entity,
                    float start[3],
                    float rel[3],
                    void *user_data)
{
  RigData *data = user_data;

  data->origin[0] = start[0] - rel[0];
  data->origin[1] = start[1] - rel[1];
  data->origin[2] = start[2] - rel[2];

  update_camera_position (data);
}

static void
set_play_mode_enabled (RigData *data, CoglBool enabled)
{
  data->play_mode = enabled;

  if (data->play_mode)
    {
      data->enable_dof = TRUE;
      data->debug_pick_ray = 0;
    }
  else
    {
      data->enable_dof = FALSE;
      data->debug_pick_ray = 1;
    }

  rut_shell_queue_redraw (data->ctx->shell);
}

void
rig_set_selected_entity (RigData *data,
                         RutEntity *entity)
{
  data->selected_entity = entity;

  if (entity == NULL)
    rut_tool_update (data->tool, NULL);
  else if (entity == data->light_handle)
    data->selected_entity = data->light;

  rut_shell_queue_redraw (data->ctx->shell);
  update_inspector (data);
}

static RutInputEventStatus
main_input_cb (RutInputEvent *event,
               void *user_data)
{
  RigData *data = user_data;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      RutMotionEventAction action = rut_motion_event_get_action (event);
      RutModifierState modifiers = rut_motion_event_get_modifier_state (event);
      float x = rut_motion_event_get_x (event);
      float y = rut_motion_event_get_y (event);
      RutButtonState state;

      rut_camera_transform_window_coordinate (data->editor_camera_component,
                                              &x, &y);

      state = rut_motion_event_get_button_state (event);

      if (action == RUT_MOTION_EVENT_ACTION_DOWN &&
          state == RUT_BUTTON_STATE_1)
        {
          /* pick */
          RutCamera *camera;
          float ray_position[3], ray_direction[3], screen_pos[2],
                z_far, z_near;
          const float *viewport;
          const CoglMatrix *inverse_projection;
          //CoglMatrix *camera_transform;
          const CoglMatrix *camera_view;
          CoglMatrix camera_transform;
          RutObject *picked_entity;

          camera = rut_entity_get_component (data->editor_camera,
                                             RUT_COMPONENT_TYPE_CAMERA);
          viewport = rut_camera_get_viewport (RUT_CAMERA (camera));
          z_near = rut_camera_get_near_plane (RUT_CAMERA (camera));
          z_far = rut_camera_get_far_plane (RUT_CAMERA (camera));
          inverse_projection =
            rut_camera_get_inverse_projection (RUT_CAMERA (camera));

#if 0
          camera_transform = rut_entity_get_transform (data->editor_camera);
#else
          camera_view = rut_camera_get_view_transform (camera);
          cogl_matrix_get_inverse (camera_view, &camera_transform);
#endif

          screen_pos[0] = x;
          screen_pos[1] = y;

          rut_util_create_pick_ray (viewport,
                                    inverse_projection,
                                    &camera_transform,
                                    screen_pos,
                                    ray_position,
                                    ray_direction);

          if (data->debug_pick_ray)
            {
              float x1 = 0, y1 = 0, z1 = z_near, w1 = 1;
              float x2 = 0, y2 = 0, z2 = z_far, w2 = 1;
              float len;

              if (data->picking_ray)
                cogl_object_unref (data->picking_ray);

              /* FIXME: This is a hack, we should intersect the ray with
               * the far plane to decide how long the debug primitive
               * should be */
              cogl_matrix_transform_point (&camera_transform,
                                           &x1, &y1, &z1, &w1);
              cogl_matrix_transform_point (&camera_transform,
                                           &x2, &y2, &z2, &w2);
              len = z2 - z1;

              data->picking_ray = create_picking_ray (data,
                                                      rut_camera_get_framebuffer (camera),
                                                      ray_position,
                                                      ray_direction,
                                                      len);
            }

          picked_entity = pick (data,
                                camera,
                                rut_camera_get_framebuffer (camera),
                                ray_position,
                                ray_direction);

          rig_set_selected_entity (data, picked_entity);

          /* If we have selected an entity then initiate a grab so the
           * entity can be moved with the mouse...
           */
          if (data->selected_entity)
            {
              if (!translate_grab_entity (data,
                                          rut_input_event_get_camera (event),
                                          data->selected_entity,
                                          rut_motion_event_get_x (event),
                                          rut_motion_event_get_y (event),
                                          entity_translate_cb,
                                          entity_translate_done_cb,
                                          data))
                return RUT_INPUT_EVENT_STATUS_UNHANDLED;
            }

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (action == RUT_MOTION_EVENT_ACTION_DOWN &&
               state == RUT_BUTTON_STATE_2 &&
               ((modifiers & RUT_MODIFIER_SHIFT_ON) == 0))
        {
          //data->saved_rotation = *rut_entity_get_rotation (data->editor_camera);
          data->saved_rotation = *rut_entity_get_rotation (data->editor_camera_rotate);

          cogl_quaternion_init_identity (&data->arcball.q_drag);

          //rut_arcball_mouse_down (&data->arcball, data->width - x, y);
          rut_arcball_mouse_down (&data->arcball, data->main_width - x, data->main_height - y);
          //g_print ("Arcball init, mouse = (%d, %d)\n", (int)(data->width - x), (int)(data->height - y));

          //print_quaternion (&data->saved_rotation, "Saved Quaternion");
          //print_quaternion (&data->arcball.q_drag, "Arcball Initial Quaternion");
          //data->button_down = TRUE;

          data->grab_x = x;
          data->grab_y = y;
          memcpy (data->saved_origin, data->origin, sizeof (data->origin));

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (action == RUT_MOTION_EVENT_ACTION_MOVE &&
               state == RUT_BUTTON_STATE_2 &&
               modifiers & RUT_MODIFIER_SHIFT_ON)
        {
          if (!translate_grab_entity (data,
                                      rut_input_event_get_camera (event),
                                      data->editor_camera_to_origin,
                                      rut_motion_event_get_x (event),
                                      rut_motion_event_get_y (event),
                                      scene_translate_cb,
                                      NULL,
                                      data))
            return RUT_INPUT_EVENT_STATUS_UNHANDLED;
#if 0
          float origin[3] = {0, 0, 0};
          float unit_x[3] = {1, 0, 0};
          float unit_y[3] = {0, 1, 0};
          float x_vec[3];
          float y_vec[3];
          float dx;
          float dy;
          float translation[3];

          rut_entity_get_transformed_position (data->editor_camera,
                                               origin);
          rut_entity_get_transformed_position (data->editor_camera,
                                               unit_x);
          rut_entity_get_transformed_position (data->editor_camera,
                                               unit_y);

          x_vec[0] = origin[0] - unit_x[0];
          x_vec[1] = origin[1] - unit_x[1];
          x_vec[2] = origin[2] - unit_x[2];

            {
              CoglMatrix transform;
              rut_graphable_get_transform (data->editor_camera, &transform);
              cogl_debug_matrix_print (&transform);
            }
          g_print (" =========================== x_vec = %f, %f, %f\n",
                   x_vec[0], x_vec[1], x_vec[2]);

          y_vec[0] = origin[0] - unit_y[0];
          y_vec[1] = origin[1] - unit_y[1];
          y_vec[2] = origin[2] - unit_y[2];

          //dx = (x - data->grab_x) * (data->editor_camera_z / 100.0f);
          //dy = -(y - data->grab_y) * (data->editor_camera_z / 100.0f);
          dx = (x - data->grab_x);
          dy = -(y - data->grab_y);

          translation[0] = dx * x_vec[0];
          translation[1] = dx * x_vec[1];
          translation[2] = dx * x_vec[2];

          translation[0] += dy * y_vec[0];
          translation[1] += dy * y_vec[1];
          translation[2] += dy * y_vec[2];

          data->origin[0] = data->saved_origin[0] + translation[0];
          data->origin[1] = data->saved_origin[1] + translation[1];
          data->origin[2] = data->saved_origin[2] + translation[2];

          update_camera_position (data);

          g_print ("Translate %f %f dx=%f, dy=%f\n",
                   x - data->grab_x,
                   y - data->grab_y,
                   dx, dy);
#endif
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
      else if (action == RUT_MOTION_EVENT_ACTION_MOVE &&
               state == RUT_BUTTON_STATE_2 &&
               ((modifiers & RUT_MODIFIER_SHIFT_ON) == 0))
        {
          CoglQuaternion new_rotation;

          //if (!data->button_down)
          //  break;

          //rut_arcball_mouse_motion (&data->arcball, data->width - x, y);
          rut_arcball_mouse_motion (&data->arcball, data->main_width - x, data->main_height - y);
#if 0
          g_print ("Arcball motion, center=%f,%f mouse = (%f, %f)\n",
                   data->arcball.center[0],
                   data->arcball.center[1],
                   x, y);
#endif

          cogl_quaternion_multiply (&new_rotation,
                                    &data->saved_rotation,
                                    &data->arcball.q_drag);

          //rut_entity_set_rotation (data->editor_camera, &new_rotation);
          rut_entity_set_rotation (data->editor_camera_rotate, &new_rotation);

          //print_quaternion (&new_rotation, "New Rotation");

          //print_quaternion (&data->arcball.q_drag, "Arcball Quaternion");

          rut_shell_queue_redraw (data->ctx->shell);

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }

    }
#ifdef RIG_EDITOR_ENABLED
  else if (!_rig_in_device_mode &&
           rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_KEY &&
           rut_key_event_get_action (event) == RUT_KEY_EVENT_ACTION_UP)
    {
      switch (rut_key_event_get_keysym (event))
        {
        case RUT_KEY_minus:
          if (data->editor_camera_z)
            data->editor_camera_z *= 1.2f;
          else
            data->editor_camera_z = 0.1;

          update_camera_position (data);
          break;
        case RUT_KEY_equal:
          data->editor_camera_z *= 0.8;
          update_camera_position (data);
          break;
        case RUT_KEY_p:
          set_play_mode_enabled (data, !data->play_mode);
          break;
        case RUT_KEY_Delete:
          if (data->selected_entity)
            {
              rig_undo_journal_delete_entity_and_log (data->undo_journal,
                                                      data->selected_entity);
              rig_set_selected_entity (data, NULL);
            }
          break;
        }
    }
#endif /* RIG_EDITOR_ENABLED */

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static RutInputEventStatus
device_mode_grab_input_cb (RutInputEvent *event, void *user_data)
{
  RigData *data = user_data;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      RutMotionEventAction action = rut_motion_event_get_action (event);

      switch (action)
        {
        case RUT_MOTION_EVENT_ACTION_UP:
          rut_shell_ungrab_input (data->ctx->shell,
                                  device_mode_grab_input_cb,
                                  user_data);
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        case RUT_MOTION_EVENT_ACTION_MOVE:
          {
            float x = rut_motion_event_get_x (event);
            float dx = x - data->grab_x;
            CoglFramebuffer *fb = COGL_FRAMEBUFFER (data->onscreen);
            float progression = dx / cogl_framebuffer_get_width (fb);

            rut_timeline_set_progress (data->timeline,
                                       data->grab_progress + progression);

            rut_shell_queue_redraw (data->ctx->shell);
            return RUT_INPUT_EVENT_STATUS_HANDLED;
          }
        default:
          return RUT_INPUT_EVENT_STATUS_UNHANDLED;
        }
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static RutInputEventStatus
device_mode_input_cb (RutInputEvent *event,
                      void *user_data)
{
  RigData *data = user_data;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      RutMotionEventAction action = rut_motion_event_get_action (event);
      RutButtonState state = rut_motion_event_get_button_state (event);

      if (action == RUT_MOTION_EVENT_ACTION_DOWN &&
          state == RUT_BUTTON_STATE_1)
        {
            data->grab_x = rut_motion_event_get_x (event);
            data->grab_y = rut_motion_event_get_y (event);
            data->grab_progress = rut_timeline_get_progress (data->timeline);

            /* TODO: Add rut_shell_implicit_grab_input() that handles releasing
             * the grab for you */
            rut_shell_grab_input (data->ctx->shell,
                                  rut_input_event_get_camera (event),
                                  device_mode_grab_input_cb, data);
            return RUT_INPUT_EVENT_STATUS_HANDLED;

        }
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

static RutInputEventStatus
editor_input_region_cb (RutInputRegion *region,
                      RutInputEvent *event,
                      void *user_data)
{
#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    return main_input_cb (event, user_data);
  else
#endif
    return device_mode_input_cb (event, user_data);
}

void
matrix_view_2d_in_frustum (CoglMatrix *matrix,
                           float left,
                           float right,
                           float bottom,
                           float top,
                           float z_near,
                           float z_2d,
                           float width_2d,
                           float height_2d)
{
  float left_2d_plane = left / z_near * z_2d;
  float right_2d_plane = right / z_near * z_2d;
  float bottom_2d_plane = bottom / z_near * z_2d;
  float top_2d_plane = top / z_near * z_2d;

  float width_2d_start = right_2d_plane - left_2d_plane;
  float height_2d_start = top_2d_plane - bottom_2d_plane;

  /* Factors to scale from framebuffer geometry to frustum
   * cross-section geometry. */
  float width_scale = width_2d_start / width_2d;
  float height_scale = height_2d_start / height_2d;

  //cogl_matrix_translate (matrix,
  //                       left_2d_plane, top_2d_plane, -z_2d);
  cogl_matrix_translate (matrix,
                         left_2d_plane, top_2d_plane, 0);

  cogl_matrix_scale (matrix, width_scale, -height_scale, width_scale);
}

/* Assuming a symmetric perspective matrix is being used for your
 * projective transform then for a given z_2d distance within the
 * projective frustrum this convenience function determines how
 * we can use an entity transform to move from a normalized coordinate
 * space with (0,0) in the center of the screen to a non-normalized
 * 2D coordinate space with (0,0) at the top-left of the screen.
 *
 * Note: It assumes the viewport aspect ratio matches the desired
 * aspect ratio of the 2d coordinate space which is why we only
 * need to know the width of the 2d coordinate space.
 *
 */
void
get_entity_transform_for_2d_view (float fov_y,
                                  float aspect,
                                  float z_near,
                                  float z_2d,
                                  float width_2d,
                                  float *dx,
                                  float *dy,
                                  float *dz,
                                  CoglQuaternion *rotation,
                                  float *scale)
{
  float top = z_near * tan (fov_y * G_PI / 360.0);
  float left = -top * aspect;
  float right = top * aspect;

  float left_2d_plane = left / z_near * z_2d;
  float right_2d_plane = right / z_near * z_2d;
  float top_2d_plane = top / z_near * z_2d;

  float width_2d_start = right_2d_plane - left_2d_plane;

  *dx = left_2d_plane;
  *dy = top_2d_plane;
  *dz = 0;
  //*dz = -z_2d;

  /* Factors to scale from framebuffer geometry to frustum
   * cross-section geometry. */
  *scale = width_2d_start / width_2d;

  cogl_quaternion_init_from_z_rotation (rotation, 180);
}

static void
matrix_view_2d_in_perspective (CoglMatrix *matrix,
                               float fov_y,
                               float aspect,
                               float z_near,
                               float z_2d,
                               float width_2d,
                               float height_2d)
{
  float top = z_near * tan (fov_y * G_PI / 360.0);

  matrix_view_2d_in_frustum (matrix,
                             -top * aspect,
                             top * aspect,
                             -top,
                             top,
                             z_near,
                             z_2d,
                             width_2d,
                             height_2d);
}

static void
allocate_main_area (RigData *data)
{
  float screen_aspect;
  float main_aspect;
  float device_scale;

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      rut_bevel_get_size (data->main_area_bevel, &data->main_width, &data->main_height);
      if (data->main_width <= 0)
        data->main_width = 10;
      if (data->main_height <= 0)
        data->main_height = 10;
    }
  else
#endif /* RIG_EDITOR_ENABLED */
    {
      CoglFramebuffer *fb = COGL_FRAMEBUFFER (data->onscreen);
      data->main_width = cogl_framebuffer_get_width (fb);
      data->main_height = cogl_framebuffer_get_height (fb);
    }

  /* Update the window camera */
  rut_camera_set_projection_mode (data->camera, RUT_PROJECTION_ORTHOGRAPHIC);
  rut_camera_set_orthographic_coordinates (data->camera,
                                           0, 0, data->width, data->height);
  rut_camera_set_near_plane (data->camera, -1);
  rut_camera_set_far_plane (data->camera, 100);

  rut_camera_set_viewport (data->camera, 0, 0, data->width, data->height);

  screen_aspect = data->device_width / data->device_height;
  main_aspect = data->main_width / data->main_height;

  if (screen_aspect < main_aspect) /* screen is slimmer and taller than the main area */
    {
      data->screen_area_height = data->main_height;
      data->screen_area_width = data->screen_area_height * screen_aspect;

      rut_entity_set_translate (data->editor_camera_screen_pos,
                                -(data->main_width / 2.0) + (data->screen_area_width / 2.0),
                                0, 0);
    }
  else
    {
      data->screen_area_width = data->main_width;
      data->screen_area_height = data->screen_area_width / screen_aspect;

      rut_entity_set_translate (data->editor_camera_screen_pos,
                                0,
                                -(data->main_height / 2.0) + (data->screen_area_height / 2.0),
                                0);
    }

  /* NB: We know the screen area matches the device aspect ratio so we can use
   * a uniform scale here... */
  device_scale = data->screen_area_width / data->device_width;

  rut_entity_set_scale (data->editor_camera_dev_scale, 1.0 / device_scale);

  /* Setup projection for main content view */
  {
    float fovy = 10; /* y-axis field of view */
    float aspect = (float)data->main_width/(float)data->main_height;
    float z_near = 10; /* distance to near clipping plane */
    float z_far = 100; /* distance to far clipping plane */
    float x = 0, y = 0, z_2d = 30, w = 1;
    CoglMatrix inverse;

    data->z_2d = z_2d; /* position to 2d plane */

    cogl_matrix_init_identity (&data->main_view);
    matrix_view_2d_in_perspective (&data->main_view,
                                   fovy, aspect, z_near, data->z_2d,
                                   data->main_width,
                                   data->main_height);

    rut_camera_set_projection_mode (data->editor_camera_component,
                                    RUT_PROJECTION_PERSPECTIVE);
    rut_camera_set_field_of_view (data->editor_camera_component, fovy);
    rut_camera_set_near_plane (data->editor_camera_component, z_near);
    rut_camera_set_far_plane (data->editor_camera_component, z_far);

    /* Handle the z_2d translation by changing the length of the
     * camera's armature.
     */
    cogl_matrix_get_inverse (&data->main_view,
                             &inverse);
    cogl_matrix_transform_point (&inverse,
                                 &x, &y, &z_2d, &w);

    data->editor_camera_z = z_2d / device_scale;
    rut_entity_set_translate (data->editor_camera_armature, 0, 0, data->editor_camera_z);
    //rut_entity_set_translate (data->editor_camera_armature, 0, 0, 0);

    {
      float dx, dy, dz, scale;
      CoglQuaternion rotation;

      get_entity_transform_for_2d_view (fovy,
                                        aspect,
                                        z_near,
                                        data->z_2d,
                                        data->main_width,
                                        &dx,
                                        &dy,
                                        &dz,
                                        &rotation,
                                        &scale);

      rut_entity_set_translate (data->editor_camera_2d_view, -dx, -dy, -dz);
      rut_entity_set_rotation (data->editor_camera_2d_view, &rotation);
      rut_entity_set_scale (data->editor_camera_2d_view, 1.0 / scale);
    }
  }

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      rut_arcball_init (&data->arcball,
                        data->main_width / 2,
                        data->main_height / 2,
                        sqrtf (data->main_width *
                               data->main_width +
                               data->main_height *
                               data->main_height) / 2);
    }
#endif /* RIG_EDITOR_ENABLED */
}

static void
allocate (RigData *data)
{
  //data->main_width = data->width - data->left_bar_width - data->right_bar_width;
  //data->main_height = data->height - data->top_bar_height - data->bottom_bar_height;

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    rut_split_view_set_size (data->splits[0], data->width, data->height);
#endif

  allocate_main_area (data);
}

static void
data_onscreen_resize (CoglOnscreen *onscreen,
                      int width,
                      int height,
                      void *user_data)
{
  RigData *data = user_data;

  data->width = width;
  data->height = height;

  rut_property_dirty (&data->ctx->property_ctx, &data->properties[RIG_DATA_PROP_WIDTH]);
  rut_property_dirty (&data->ctx->property_ctx, &data->properties[RIG_DATA_PROP_HEIGHT]);

  allocate (data);
}

static void
camera_viewport_binding_cb (RutProperty *target_property,
                            RutProperty *source_property,
                            void *user_data)
{
  RigData *data = user_data;
  float x, y, z, width, height;

  x = y = z = 0;
  rut_graphable_fully_transform_point (data->main_area_bevel,
                                       data->camera,
                                       &x, &y, &z);

  data->main_x = x;
  data->main_y = y;

  x = RUT_UTIL_NEARBYINT (x);
  y = RUT_UTIL_NEARBYINT (y);

  rut_bevel_get_size (data->main_area_bevel, &width, &height);

  /* XXX: We round down here since that's currently what
   * rig-bevel.c:_rut_bevel_paint() does too. */
  width = (int)width;
  height = (int)height;

  rut_camera_set_viewport (data->editor_camera_component,
                           x, y, width, height);

  rut_input_region_set_rectangle (data->editor_input_region,
                                  x, y,
                                  x + width,
                                  y + height);

  allocate_main_area (data);
}

typedef struct _AssetInputClosure
{
  RutAsset *asset;
  RigData *data;
} AssetInputClosure;

static void
free_asset_input_closures (RigData *data)
{
  GList *l;

  for (l = data->asset_input_closures; l; l = l->next)
    g_slice_free (AssetInputClosure, l->data);
  g_list_free (data->asset_input_closures);
  data->asset_input_closures = NULL;
}

static RutInputEventStatus
asset_input_cb (RutInputRegion *region,
                RutInputEvent *event,
                void *user_data)
{
  AssetInputClosure *closure = user_data;
  RutInputEventStatus status = RUT_INPUT_EVENT_STATUS_UNHANDLED;
  RutAsset *asset = closure->asset;
  RigData *data = closure->data;
  RutEntity *entity;
  RutMaterial *material;
  CoglTexture *texture;
  RutObject *geom;
  RutShape *shape;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_DOWN)
        {
          RutAssetType type = rut_asset_get_type (asset);

          if (data->selected_entity)
            entity = data->selected_entity;
          else
            {
              entity = rut_entity_new (data->ctx);
              rig_undo_journal_add_entity_and_log (data->undo_journal,
                                                   data->scene,
                                                   entity);
              rig_set_selected_entity (data, entity);
            }

          switch (type)
            {
            case RUT_ASSET_TYPE_TEXTURE:
            case RUT_ASSET_TYPE_NORMAL_MAP:
            case RUT_ASSET_TYPE_ALPHA_MASK:
              {
                int width, height;

                material =
                  rut_entity_get_component (entity, RUT_COMPONENT_TYPE_MATERIAL);
                if (material)
                  {
                    if (type == RUT_ASSET_TYPE_TEXTURE)
                      rut_material_set_texture_asset (material, asset);
                    else if (type == RUT_ASSET_TYPE_NORMAL_MAP)
                      rut_material_set_normal_map_asset (material, asset);
                    else if (type == RUT_ASSET_TYPE_ALPHA_MASK)
                      rut_material_set_alpha_mask_asset (material, asset);
                  }
                else
                  {
                    material = rut_material_new (data->ctx, asset);
                    rut_entity_add_component (entity, material);
                  }

                texture = rut_asset_get_texture (asset);
                width = cogl_texture_get_width (texture);
                height = cogl_texture_get_height (texture);

                geom = rut_entity_get_component (entity,
                                                 RUT_COMPONENT_TYPE_GEOMETRY);
                if (!geom)
                  {
                    shape = rut_shape_new (data->ctx, TRUE, width, height);
                    rut_entity_add_component (entity, shape);
                    geom = shape;
                  }

                if (type == RUT_ASSET_TYPE_TEXTURE)
                  {
                    if (rut_object_get_type (geom) == &rut_shape_type)
                      {
                        if (rut_object_get_type (geom) == &rut_shape_type)
                          rut_shape_set_texture_size (RUT_SHAPE (geom),
                                                      width, height);
                      }
                    else if (rut_object_get_type (geom) == &rut_diamond_type)
                      {
                        RutDiamond *diamond = geom;
                        float size = rut_diamond_get_size (diamond);

                        rut_entity_remove_component (entity, geom);
                        diamond = rut_diamond_new (data->ctx, size, width, height);
                      }
                  }

                status = RUT_INPUT_EVENT_STATUS_HANDLED;
                break;
              }
            case RUT_ASSET_TYPE_PLY_MODEL:
              {
                RutModel *model;
                float x_range, y_range, z_range, max_range;

                geom = rut_entity_get_component (entity,
                                                 RUT_COMPONENT_TYPE_GEOMETRY);

                if (geom && rut_object_get_type (geom) == &rut_model_type)
                  {
                    model = geom;
                    if (rut_model_get_asset (model) == asset)
                      {
                        status = RUT_INPUT_EVENT_STATUS_HANDLED;
                        break;
                      }
                  }
                else if (geom)
                  rut_entity_remove_component (entity, geom);

                /* XXX: For now we forcibly remove any material from
                 * the entity when adding a ply model geometry
                 * component since it's likely the model doesn't have
                 * texture coordinates and if the material has an
                 * associated texture with a transparent top-left
                 * pixel the model won't be visible. */
                material =
                  rut_entity_get_component (entity, RUT_COMPONENT_TYPE_MATERIAL);
                if (material)
                  rut_entity_remove_component (entity, material);

                model = rut_model_new_from_asset (data->ctx, asset);
                rut_entity_add_component (entity, model);

                x_range = model->max_x - model->min_x;
                y_range = model->max_y - model->min_y;
                z_range = model->max_z - model->min_z;

                max_range = x_range;
                if (y_range > max_range)
                  max_range = y_range;
                if (z_range > max_range)
                  max_range = z_range;

                rut_entity_set_scale (entity, 200.0 / max_range);

                status = RUT_INPUT_EVENT_STATUS_HANDLED;
                break;
              }
            case RUT_ASSET_TYPE_BUILTIN:
              if (asset == data->text_builtin_asset)
                {
                  RutText *text;
                  CoglColor color;

                  geom = rut_entity_get_component (entity,
                                                   RUT_COMPONENT_TYPE_GEOMETRY);

                  if (geom && rut_object_get_type (geom) == &rut_text_type)
                    return RUT_INPUT_EVENT_STATUS_HANDLED;
                  else if (geom)
                    rut_entity_remove_component (entity, geom);

                  text = rut_text_new_with_text (data->ctx, "Sans 60px", "text");
                  cogl_color_init_from_4f (&color, 1, 1, 1, 1);
                  rut_text_set_color (text, &color);
                  rut_entity_add_component (entity, text);

                  status = RUT_INPUT_EVENT_STATUS_HANDLED;
                }
              else if (asset == data->circle_builtin_asset)
                {
                  RutShape *shape;
                  int tex_width = 200, tex_height = 200;

                  geom = rut_entity_get_component (entity,
                                                   RUT_COMPONENT_TYPE_GEOMETRY);

                  if (geom && rut_object_get_type (geom) == &rut_shape_type)
                    {
                      status = RUT_INPUT_EVENT_STATUS_HANDLED;
                      break;
                    }
                  else if (geom)
                    rut_entity_remove_component (entity, geom);

                  material =
                    rut_entity_get_component (entity, RUT_COMPONENT_TYPE_MATERIAL);

                  if (material)
                    {
                      RutAsset *texture_asset =
                        rut_material_get_texture_asset (material);
                      if (texture_asset)
                        {
                          CoglTexture *texture = rut_asset_get_texture (texture_asset);
                          tex_width = cogl_texture_get_width (texture);
                          tex_height = cogl_texture_get_height (texture);
                        }
                    }

                  shape = rut_shape_new (data->ctx, TRUE, tex_width, tex_height);
                  rut_entity_add_component (entity, shape);

                  status = RUT_INPUT_EVENT_STATUS_HANDLED;
                }
              else if (asset == data->diamond_builtin_asset)
                {
                  RutDiamond *diamond;
                  int tex_width = 200, tex_height = 200;

                  geom = rut_entity_get_component (entity,
                                                   RUT_COMPONENT_TYPE_GEOMETRY);

                  if (geom && rut_object_get_type (geom) == &rut_diamond_type)
                    {
                      status = RUT_INPUT_EVENT_STATUS_HANDLED;
                      break;
                    }
                  else if (geom)
                    rut_entity_remove_component (entity, geom);

                  material =
                    rut_entity_get_component (entity, RUT_COMPONENT_TYPE_MATERIAL);

                  if (material)
                    {
                      RutAsset *texture_asset =
                        rut_material_get_texture_asset (material);
                      if (texture_asset)
                        {
                          CoglTexture *texture = rut_asset_get_texture (texture_asset);
                          tex_width = cogl_texture_get_width (texture);
                          tex_height = cogl_texture_get_height (texture);
                        }
                    }

                  diamond = rut_diamond_new (data->ctx, 200, tex_width, tex_height);
                  rut_entity_add_component (entity, diamond);

                  status = RUT_INPUT_EVENT_STATUS_HANDLED;
                }

              break;
            }

          if (status == RUT_INPUT_EVENT_STATUS_HANDLED)
            {
              rig_dirty_entity_pipelines (entity);
              update_inspector (data);
              rut_shell_queue_redraw (data->ctx->shell);
            }
        }
    }

  return status;
}

static CoglBool
asset_matches_search (RutAsset *asset,
                      const char *search)
{
  const GList *inferred_tags;
  char **tags;
  const char *path;
  int i;

  if (!search)
    return TRUE;

  inferred_tags = rut_asset_get_inferred_tags (asset);
  tags = g_strsplit_set (search, " \t", 0);

  path = rut_asset_get_path (asset);
  if (path)
    {
      if (strstr (path, search))
        return TRUE;
    }

  for (i = 0; tags[i]; i++)
    {
      GList *l;
      CoglBool found = FALSE;

      for (l = inferred_tags; l; l = l->next)
        {
          if (strcmp (tags[i], l->data) == 0)
            {
              found = TRUE;
              break;
            }
        }

      if (!found)
        {
          g_strfreev (tags);
          return FALSE;
        }
    }

  g_strfreev (tags);
  return TRUE;
}

static void
add_asset_icon (RigData *data,
                RutAsset *asset,
                float y_pos)
{
  AssetInputClosure *closure;
  CoglTexture *texture;
  RutImage *image;
  RutInputRegion *region;
  RutTransform *transform;

  closure = g_slice_new (AssetInputClosure);
  closure->asset = asset;
  closure->data = data;

  transform = rut_transform_new (data->ctx, NULL);

  texture = rut_asset_get_texture (asset);
  if (texture)
    {
      image = rut_image_new (data->ctx, texture);
      rut_sizable_set_size (image, 100, 100);
      rut_graphable_add_child (transform, image);
      rut_refable_unref (image);
    }
  else
    {
      char *basename = g_path_get_basename (rut_asset_get_path (asset));
      RutText *text = rut_text_new_with_text (data->ctx, NULL, basename);
      rut_sizable_set_size (text, 100, 100);
      g_free (basename);
      rut_graphable_add_child (transform, text);
      rut_refable_unref (text);
    }

  region = rut_input_region_new_rectangle (0, 0, 100, 100,
                                           asset_input_cb,
                                           closure);
  rut_graphable_add_child (transform, region);
  rut_refable_unref (region);

  rut_graphable_add_child (data->assets_list, transform);
  rut_refable_unref (transform);

  /* XXX: It could be nicer to have some form of weak pointer
   * mechanism to manage the lifetime of these closures... */
  data->asset_input_closures = g_list_prepend (data->asset_input_closures,
                                               closure);

  rut_transform_translate (transform, 10, y_pos, 0);

  //rut_input_region_set_graphable (region, nine_slice);
}

static CoglBool
rig_search_asset_list (RigData *data, const char *search)
{
  GList *l;
  int i;
  RutObject *doc_node;
  CoglBool found = FALSE;

  if (data->assets_list)
    {
      rut_graphable_remove_child (data->assets_list);
      free_asset_input_closures (data);
    }

  data->assets_list = rut_graph_new (data->ctx, NULL);

  if (data->transparency_grid)
    rut_graphable_add_child (data->assets_list, data->transparency_grid);

  doc_node = rut_ui_viewport_get_doc_node (data->assets_vp);
  rut_graphable_add_child (doc_node, data->assets_list);
  rut_refable_unref (data->assets_list);
  data->assets_list_tail_pos = 70;

  for (l = data->assets, i= 0; l; l = l->next, i++)
    {
      RutAsset *asset = l->data;

      if (!asset_matches_search (asset, search))
        continue;

      found = TRUE;
      add_asset_icon (data, asset, data->assets_list_tail_pos);
      data->assets_list_tail_pos += 110;

      rut_ui_viewport_set_doc_height (data->assets_vp,
                                      data->assets_list_tail_pos);
    }

  return found;
}

static void
asset_search_update_cb (RutText *text,
                        void *user_data)
{
  if (!rig_search_asset_list (user_data, rut_text_get_text (text)))
    rig_search_asset_list (user_data, NULL);
}


#ifdef RIG_EDITOR_ENABLED

static RutImage *
load_transparency_grid (RutContext *ctx)
{
  GError *error = NULL;
  CoglTexture *texture =
    rut_load_texture_from_data_file (ctx, "transparency-grid.png", &error);
  RutImage *ret;

  if (texture == NULL)
    {
      g_warning ("Failed to load transparency-grid.png: %s",
                 error->message);
      g_error_free (error);
    }
  else
    {
      ret = rut_image_new (ctx, texture);

      rut_image_set_draw_mode (ret, RUT_IMAGE_DRAW_MODE_REPEAT);
      rut_sizable_set_size (ret, 1000000.0f, 1000000.0f);

      cogl_object_unref (texture);
    }

  return ret;
}

#endif /* RIG_EDITOR_ENABLED */

/* These should be sorted in descending order of size to
 * avoid gaps due to attributes being naturally aligned. */
static RutPLYAttribute ply_attributes[] =
{
  {
    .name = "cogl_position_in",
    .properties = {
      { "x" },
      { "y" },
      { "z" },
    },
    .n_properties = 3,
    .min_components = 1,
  },
  {
    .name = "cogl_normal_in",
    .properties = {
      { "nx" },
      { "ny" },
      { "nz" },
    },
    .n_properties = 3,
    .min_components = 3,
    .pad_n_components = 3,
    .pad_type = RUT_ATTRIBUTE_TYPE_FLOAT,
  },
  {
    .name = "cogl_tex_coord0_in",
    .properties = {
      { "s" },
      { "t" },
      { "r" },
    },
    .n_properties = 3,
    .min_components = 2,
  },
  {
    .name = "tangent",
    .properties = {
      { "tanx" },
      { "tany" },
      { "tanz" }
    },
    .n_properties = 3,
    .min_components = 3,
    .pad_n_components = 3,
    .pad_type = RUT_ATTRIBUTE_TYPE_FLOAT,
  },
  {
    .name = "cogl_color_in",
    .properties = {
      { "red" },
      { "green" },
      { "blue" },
      { "alpha" }
    },
    .n_properties = 4,
    .normalized = TRUE,
    .min_components = 3,
  }
};

static void
init (RutShell *shell, void *user_data)
{
  RigData *data = user_data;
  CoglFramebuffer *fb;
  float vector3[3];
  int i;
  char *full_path;
  CoglError *error = NULL;
  CoglTexture2D *color_buffer;
  CoglColor color;
  RutModel *model;
  RutMaterial *material;
  RutLight *light;
  RutCamera *camera;
  CoglColor top_bar_ref_color, main_area_ref_color, right_bar_ref_color;

  /* A unit test for the list_splice/list_unsplice functions */
#if 0
  _rut_test_list_splice ();
#endif

  cogl_matrix_init_identity (&data->identity);

  for (i = 0; i < RIG_DATA_N_PROPS; i++)
    rut_property_init (&data->properties[i],
                       &rut_data_property_specs[i],
                       data);

  data->timeline = rut_timeline_new (data->ctx, 20.0);
  rut_timeline_stop (data->timeline);

  data->timeline_elapsed =
    rut_introspectable_lookup_property (data->timeline, "elapsed");
  data->timeline_progress =
    rut_introspectable_lookup_property (data->timeline, "progress");

  data->circle_texture = rut_create_circle_texture (data->ctx,
                                                    CIRCLE_TEX_RADIUS,
                                                    CIRCLE_TEX_PADDING);

  data->scene = rut_graph_new (data->ctx, NULL);

  data->device_width = DEVICE_WIDTH;
  data->device_height = DEVICE_HEIGHT;
  cogl_color_init_from_4f (&data->background_color, 0, 0, 0, 1);

#ifndef __ANDROID__
  if (_rig_ui_filename)
    {
      struct stat st;

      stat (_rig_ui_filename, &st);
      if (S_ISREG (st.st_mode))
        rig_load (data, _rig_ui_filename);
    }
#endif

  data->journal = rig_journal_new ();

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    data->onscreen = cogl_onscreen_new (data->ctx->cogl_context, 1000, 700);
  else
#endif
    data->onscreen = cogl_onscreen_new (data->ctx->cogl_context,
                                        data->device_width / 2, data->device_height / 2);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      cogl_onscreen_set_resizable (data->onscreen, TRUE);
      cogl_onscreen_add_resize_handler (data->onscreen, data_onscreen_resize, data);
    }
#endif

  cogl_onscreen_show (data->onscreen);

  fb = COGL_FRAMEBUFFER (data->onscreen);
  data->width = cogl_framebuffer_get_width (fb);
  data->height  = cogl_framebuffer_get_height (fb);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    data->undo_journal = rig_undo_journal_new (data);

  /* Create a color gradient texture that can be used for debugging
   * shadow mapping.
   *
   * XXX: This should probably simply be #ifdef DEBUG code.
   */
  if (!_rig_in_device_mode)
    {
      CoglVertexP2C4 quad[] = {
        { 0, 0, 0xff, 0x00, 0x00, 0xff },
        { 0, 200, 0x00, 0xff, 0x00, 0xff },
        { 200, 200, 0x00, 0x00, 0xff, 0xff },
        { 200, 0, 0xff, 0xff, 0xff, 0xff }
      };
      CoglOffscreen *offscreen;
      CoglPrimitive *prim =
        cogl_primitive_new_p2c4 (data->ctx->cogl_context, COGL_VERTICES_MODE_TRIANGLE_FAN, 4, quad);
      CoglPipeline *pipeline = cogl_pipeline_new (data->ctx->cogl_context);

      data->gradient = COGL_TEXTURE (
        cogl_texture_2d_new_with_size (rut_cogl_context,
                                       200, 200,
                                       COGL_PIXEL_FORMAT_ANY,
                                       NULL));

      offscreen = cogl_offscreen_new_to_texture (data->gradient);

      cogl_framebuffer_orthographic (COGL_FRAMEBUFFER (offscreen),
                                     0, 0,
                                     200,
                                     200,
                                     -1,
                                     100);
      cogl_framebuffer_clear4f (COGL_FRAMEBUFFER (offscreen),
                                COGL_BUFFER_BIT_COLOR | COGL_BUFFER_BIT_DEPTH,
                                0, 0, 0, 1);
      cogl_framebuffer_draw_primitive (COGL_FRAMEBUFFER (offscreen),
                                       pipeline,
                                       prim);
      cogl_object_unref (prim);
      cogl_object_unref (offscreen);
    }
#endif /* RIG_EDITOR_ENABLED */

  /*
   * Shadow mapping
   */

  /* Setup the shadow map */
  /* TODO: reallocate if the onscreen framebuffer is resized */
  color_buffer = cogl_texture_2d_new_with_size (rut_cogl_context,
                                                data->width * 2, data->height * 2,
                                                COGL_PIXEL_FORMAT_ANY,
                                                &error);
  if (error)
    g_critical ("could not create texture: %s", error->message);

  data->shadow_color = color_buffer;

  /* XXX: Right now there's no way to avoid allocating a color buffer. */
  data->shadow_fb =
    cogl_offscreen_new_to_texture (COGL_TEXTURE (color_buffer));
  if (data->shadow_fb == NULL)
    g_critical ("could not create offscreen buffer");

  /* retrieve the depth texture */
  cogl_framebuffer_set_depth_texture_enabled (COGL_FRAMEBUFFER (data->shadow_fb),
                                              TRUE);
  data->shadow_map =
    cogl_framebuffer_get_depth_texture (COGL_FRAMEBUFFER (data->shadow_fb));


  data->default_pipeline = cogl_pipeline_new (data->ctx->cogl_context);

  /*
   * Depth of Field
   */

  data->dof = rut_dof_effect_new (data->ctx);
  data->enable_dof = FALSE;

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      data->grid_prim = rut_create_create_grid (data->ctx,
                                                data->device_width,
                                                data->device_height,
                                                100,
                                                100);
    }
#endif

  data->circle_node_attribute =
    rut_create_circle_fan_p2 (data->ctx, 20, &data->circle_node_n_verts);

  data->device_transform = rut_transform_new (data->ctx, NULL);

  data->camera = rut_camera_new (data->ctx, COGL_FRAMEBUFFER (data->onscreen));
  rut_camera_set_clear (data->camera, FALSE);

  /* XXX: Basically just a hack for now. We should have a
   * RutShellWindow type that internally creates a RutCamera that can
   * be used when handling input events in device coordinates.
   */
  rut_shell_set_window_camera (shell, data->camera);

  /* Conceptually we rig the camera to an armature with a pivot fixed
   * at the current origin. This setup makes it straight forward to
   * model user navigation by letting us change the length of the
   * armature to handle zoom, rotating the armature to handle
   * middle-click rotating the scene with the mouse and moving the
   * position of the armature for shift-middle-click translations with
   * the mouse.
   *
   * It also simplifies things if all the viewport setup for the
   * camera is handled using entity transformations as opposed to
   * mixing entity transforms with manual camera view transforms.
   */

  data->editor_camera_to_origin = rut_entity_new (data->ctx);
  rut_graphable_add_child (data->scene, data->editor_camera_to_origin);
  rut_entity_set_label (data->editor_camera_to_origin, "rig:camera_to_origin");

  data->editor_camera_rotate = rut_entity_new (data->ctx);
  rut_graphable_add_child (data->editor_camera_to_origin, data->editor_camera_rotate);
  rut_entity_set_label (data->editor_camera_rotate, "rig:camera_rotate");

  data->editor_camera_armature = rut_entity_new (data->ctx);
  rut_graphable_add_child (data->editor_camera_rotate, data->editor_camera_armature);
  rut_entity_set_label (data->editor_camera_armature, "rig:camera_armature");

  data->editor_camera_origin_offset = rut_entity_new (data->ctx);
  rut_graphable_add_child (data->editor_camera_armature, data->editor_camera_origin_offset);
  rut_entity_set_label (data->editor_camera_origin_offset, "rig:camera_origin_offset");

  data->editor_camera_dev_scale = rut_entity_new (data->ctx);
  rut_graphable_add_child (data->editor_camera_origin_offset, data->editor_camera_dev_scale);
  rut_entity_set_label (data->editor_camera_dev_scale, "rig:camera_dev_scale");

  data->editor_camera_screen_pos = rut_entity_new (data->ctx);
  rut_graphable_add_child (data->editor_camera_dev_scale, data->editor_camera_screen_pos);
  rut_entity_set_label (data->editor_camera_screen_pos, "rig:camera_screen_pos");

  data->editor_camera_2d_view = rut_entity_new (data->ctx);
  //rut_graphable_add_child (data->editor_camera_screen_pos, data->editor_camera_2d_view); FIXME
  rut_entity_set_label (data->editor_camera_2d_view, "rig:camera_2d_view");

  data->editor_camera = rut_entity_new (data->ctx);
  //rut_graphable_add_child (data->editor_camera_2d_view, data->editor_camera); FIXME
  rut_graphable_add_child (data->editor_camera_screen_pos, data->editor_camera);
  rut_entity_set_label (data->editor_camera, "rig:camera");

  data->origin[0] = data->device_width / 2;
  data->origin[1] = data->device_height / 2;
  data->origin[2] = 0;

  rut_entity_translate (data->editor_camera_to_origin,
                        data->origin[0],
                        data->origin[1],
                        data->origin[2]);
                        //data->device_width / 2, (data->device_height / 2), 0);

  //rut_entity_rotate_z_axis (data->editor_camera_to_origin, 45);

  rut_entity_translate (data->editor_camera_origin_offset,
                        -data->device_width / 2, -(data->device_height / 2), 0);

  /* FIXME: currently we also do a z translation due to using
   * cogl_matrix_view_2d_in_perspective, we should stop using that api so we can
   * do our z_2d translation here...
   *
   * XXX: should the camera_z transform be done for the negative translate?
   */
  //device scale = 0.389062494
  data->editor_camera_z = 0.f;
  rut_entity_translate (data->editor_camera_armature, 0, 0, data->editor_camera_z);

#if 0
  {
    float pos[3] = {0, 0, 0};
    rut_entity_set_position (data->editor_camera_rig, pos);
    rut_entity_translate (data->editor_camera_rig, 100, 100, 0);
  }
#endif

  //rut_entity_translate (data->editor_camera, 100, 100, 0);

#if 0
  data->editor_camera_z = 20.f;
  vector3[0] = 0.f;
  vector3[1] = 0.f;
  vector3[2] = data->editor_camera_z;
  rut_entity_set_position (data->editor_camera, vector3);
#else
  data->editor_camera_z = 10.f;
#endif

  data->editor_camera_component = rut_camera_new (data->ctx, fb);
  rut_camera_set_clear (data->editor_camera_component, FALSE);
  rut_entity_add_component (data->editor_camera, data->editor_camera_component);
  rut_shell_add_input_camera (shell,
                              data->editor_camera_component,
                              data->scene);

  data->editor_input_region =
    rut_input_region_new_rectangle (0, 0, 0, 0, editor_input_region_cb, data);
  rut_input_region_set_hud_mode (data->editor_input_region, TRUE);
  rut_camera_add_input_region (data->editor_camera_component,
                               data->editor_input_region);


  update_camera_position (data);

  data->current_camera = data->editor_camera;

  /* Note: we currently require having exactly one scene light, so if
   * we didn't already load one we create a default light...
   */

  if (!data->light)
    {
      data->light = rut_entity_new (data->ctx);
      rut_entity_set_label (data->light, "light");

      vector3[0] = 0;
      vector3[1] = 0;
      vector3[2] = 500;
      rut_entity_set_position (data->light, vector3);

      rut_entity_rotate_x_axis (data->light, 20);
      rut_entity_rotate_y_axis (data->light, -20);

      light = rut_light_new (data->ctx);
      cogl_color_init_from_4f (&color, .2f, .2f, .2f, 1.f);
      rut_light_set_ambient (light, &color);
      cogl_color_init_from_4f (&color, .6f, .6f, .6f, 1.f);
      rut_light_set_diffuse (light, &color);
      cogl_color_init_from_4f (&color, .4f, .4f, .4f, 1.f);
      rut_light_set_specular (light, &color);

      rut_entity_add_component (data->light, light);

      rut_graphable_add_child (data->scene, data->light);
    }

  /*
   * TODO: support saving and loading the camera state for lights
   */

  camera = rut_camera_new (data->ctx, COGL_FRAMEBUFFER (data->shadow_fb));
  data->shadow_map_camera = camera;

  rut_camera_set_background_color4f (camera, 0.f, .3f, 0.f, 1.f);
  rut_camera_set_projection_mode (camera,
                                  RUT_PROJECTION_ORTHOGRAPHIC);
  rut_camera_set_orthographic_coordinates (camera,
                                           -1000, -1000, 1000, 1000);
  rut_camera_set_near_plane (camera, 1.1f);
  rut_camera_set_far_plane (camera, 1500.f);

  rut_entity_add_component (data->light, camera);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      RutPLYAttributeStatus padding_status[G_N_ELEMENTS (ply_attributes)];
      char *full_path = rut_find_data_file ("light.ply");
      GError *error = NULL;
      RutMesh *mesh;

      if (full_path == NULL)
        g_critical ("could not find model \"light.ply\"");

      mesh = rut_mesh_new_from_ply (data->ctx,
                                    full_path,
                                    ply_attributes,
                                    G_N_ELEMENTS (ply_attributes),
                                    padding_status,
                                    &error);
      RutModel *model;
      if (mesh)
        {
          model = rut_model_new_from_mesh (data->ctx, mesh);

          data->light_handle = rut_entity_new (data->ctx);
          rut_entity_set_label (data->light_handle, "rig:light_handle");
          rut_entity_add_component (data->light_handle, model);
          rut_entity_set_receive_shadow (data->light_handle, FALSE);
          rut_graphable_add_child (data->light, data->light_handle);
          rut_entity_set_scale (data->light_handle, 100);
          rut_entity_set_cast_shadow (data->light_handle, FALSE);
        }
      else
        g_critical ("could not load model %s: %s", full_path, error->message);

      g_free (full_path);
    }
#endif

  data->root =
    rut_graph_new (data->ctx,
                   //(data->main_transform = rut_transform_new (data->ctx, NULL)),
                   NULL);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      RutGraph *graph = rut_graph_new (data->ctx, NULL);
      RutTransform *transform;
      RutText *text;
      float x = 10;
      float width, height;

      cogl_color_init_from_4f (&top_bar_ref_color, 0.41, 0.41, 0.41, 1.0);
      cogl_color_init_from_4f (&main_area_ref_color, 0.22, 0.22, 0.22, 1.0);
      cogl_color_init_from_4f (&right_bar_ref_color, 0.45, 0.45, 0.45, 1.0);

      data->splits[0] =
        rut_split_view_new (data->ctx,
                            RUT_SPLIT_VIEW_SPLIT_HORIZONTAL,
                            100,
                            100,
                            NULL);

      transform = rut_transform_new (data->ctx,
                                     (text =
                                      rut_text_new_with_text (data->ctx,
                                                              NULL,
                                                              "File")), NULL);
      rut_transform_translate (transform, x, 5, 0);
      rut_graphable_add_child (graph, transform);
      rut_refable_unref (transform);
      rut_sizable_get_size (text, &width, &height);
      x += width + 30;

      transform = rut_transform_new (data->ctx,
                                     (text =
                                      rut_text_new_with_text (data->ctx,
                                                              NULL,
                                                              "Edit")), NULL);
      rut_transform_translate (transform, x, 5, 0);
      rut_graphable_add_child (graph, transform);
      rut_refable_unref (transform);
      rut_sizable_get_size (text, &width, &height);
      x += width + 30;

      transform = rut_transform_new (data->ctx,
                                     (text =
                                      rut_text_new_with_text (data->ctx,
                                                              NULL,
                                                              "Help")), NULL);
      rut_transform_translate (transform, x, 5, 0);
      rut_graphable_add_child (graph, transform);
      rut_refable_unref (transform);
      rut_sizable_get_size (text, &width, &height);
      x += width + 30;

      data->top_bar_stack =
        rut_stack_new (data->ctx, 0, 0,
                       (data->top_bar_rect =
                        rut_rectangle_new4f (data->ctx, 0, 0,
                                             0.41, 0.41, 0.41, 1)),
                       graph,
                       rut_bevel_new (data->ctx, 0, 0, &top_bar_ref_color),
                       NULL);

      rut_graphable_add_child (data->root, data->splits[0]);

      data->splits[1] = rut_split_view_new (data->ctx,
                                            RUT_SPLIT_VIEW_SPLIT_VERTICAL,
                                            100,
                                            100,
                                            NULL);

      rut_split_view_set_child0 (data->splits[0], data->top_bar_stack);
      rut_split_view_set_child1 (data->splits[0], data->splits[1]);

      data->splits[2] = rut_split_view_new (data->ctx,
                                            RUT_SPLIT_VIEW_SPLIT_HORIZONTAL,
                                            100,
                                            100,
                                            NULL);

      data->splits[3] = rut_split_view_new (data->ctx,
                                            RUT_SPLIT_VIEW_SPLIT_HORIZONTAL,
                                            100,
                                            100,
                                            NULL);

      data->splits[4] = rut_split_view_new (data->ctx,
                                            RUT_SPLIT_VIEW_SPLIT_VERTICAL,
                                            100,
                                            100,
                                            NULL);

      data->icon_bar_stack = rut_stack_new (data->ctx, 0, 0,
                                            (data->icon_bar_rect =
                                             rut_rectangle_new4f (data->ctx, 0, 0,
                                                                  0.41, 0.41, 0.41, 1)),
                                            rut_bevel_new (data->ctx, 0, 0, &top_bar_ref_color),
                                            NULL);
      rut_split_view_set_child0 (data->splits[3], data->splits[4]);
      rut_split_view_set_child1 (data->splits[3], data->icon_bar_stack);

      data->left_bar_stack = rut_stack_new (data->ctx, 0, 0,
                                            (data->left_bar_rect =
                                             rut_rectangle_new4f (data->ctx, 0, 0,
                                                                  0.57, 0.57, 0.57, 1)),
                                            (data->assets_vp =
                                             rut_ui_viewport_new (data->ctx,
                                                                  0, 0,
                                                                  NULL)),
                                            rut_bevel_new (data->ctx, 0, 0, &top_bar_ref_color),
                                            NULL);

      rut_ui_viewport_set_x_pannable (data->assets_vp, FALSE);

        {
          RutEntry *entry;
          RutText *text;
          RutTransform *transform;
          float width, min_height;

          transform = rut_transform_new (data->ctx,
                                         (entry = rut_entry_new (data->ctx)), NULL);
          rut_transform_translate (transform, 20, 10, 0);
          rut_graphable_add_child (data->assets_vp, transform);

          text = rut_entry_get_text (entry);
          rut_text_set_single_line_mode (text, TRUE);
          rut_text_set_text (text, "Search...");

          rut_text_add_text_changed_callback (text,
                                              asset_search_update_cb,
                                              data,
                                              NULL);

          rut_sizable_get_preferred_height (entry, -1, &min_height, NULL);
          rut_sizable_get_preferred_width (entry, min_height, NULL, &width);
          rut_sizable_set_size (entry, width, min_height);
        }


      data->main_area_bevel = rut_bevel_new (data->ctx, 0, 0, &main_area_ref_color),

      rut_split_view_set_child0 (data->splits[4], data->left_bar_stack);
      rut_split_view_set_child1 (data->splits[4], data->main_area_bevel);

      data->timeline_vp = rut_ui_viewport_new (data->ctx, 0, 0, NULL);
      rut_ui_viewport_set_x_pannable (data->timeline_vp, FALSE);
      rut_ui_viewport_set_x_expand (data->timeline_vp, TRUE);
      rut_ui_viewport_set_y_expand (data->timeline_vp, TRUE);

      data->bottom_bar_stack = rut_stack_new (data->ctx, 0, 0,
                                              (data->bottom_bar_rect =
                                               rut_rectangle_new4f (data->ctx, 0, 0,
                                                                    0.57, 0.57, 0.57, 1)),
                                              data->timeline_vp,
                                              NULL);

      rut_split_view_set_child0 (data->splits[2], data->splits[3]);
      rut_split_view_set_child1 (data->splits[2], data->bottom_bar_stack);

      data->inspector_box_layout =
        rut_box_layout_new (data->ctx,
                            RUT_BOX_LAYOUT_ORIENTATION_VERTICAL);

      data->right_bar_stack =
        rut_stack_new (data->ctx, 100, 100,
                       (data->right_bar_rect =
                        rut_rectangle_new4f (data->ctx, 0, 0,
                                             0.57, 0.57, 0.57, 1)),
                       (data->tool_vp =
                        rut_ui_viewport_new (data->ctx,
                                             0, 0,
                                             data->inspector_box_layout,
                                             NULL)),
                       rut_bevel_new (data->ctx, 0, 0, &right_bar_ref_color),
                       NULL);

      rut_ui_viewport_set_x_pannable (data->tool_vp, FALSE);
      rut_ui_viewport_set_y_pannable (data->tool_vp, TRUE);
      rut_ui_viewport_set_sync_widget (data->tool_vp,
                                       data->inspector_box_layout);

      rut_split_view_set_child0 (data->splits[1], data->splits[2]);
      rut_split_view_set_child1 (data->splits[1], data->right_bar_stack);

      rut_split_view_set_split_offset (data->splits[0], 30);
      rut_split_view_set_split_offset (data->splits[1], 850);
      rut_split_view_set_split_offset (data->splits[2], 500);
      rut_split_view_set_split_offset (data->splits[3], 470);
      rut_split_view_set_split_offset (data->splits[4], 150);

      data->transparency_grid = load_transparency_grid (data->ctx);
    }
#endif

  rut_shell_add_input_camera (shell, data->camera, data->root);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      RutProperty *main_area_width =
        rut_introspectable_lookup_property (data->main_area_bevel, "width");
      RutProperty *main_area_height =
        rut_introspectable_lookup_property (data->main_area_bevel, "height");

      rut_property_set_binding_by_name (data->editor_camera_component,
                                        "viewport_x",
                                        camera_viewport_binding_cb,
                                        data,
                                        /* XXX: Hack: we are currently relying on
                                         * the bevel width being redundantly re-set
                                         * at times when bevel's position may have
                                         * also changed.
                                         *
                                         * FIXME: We need a proper allocation cycle
                                         * in Rut!
                                         */
                                        main_area_width,
                                        NULL);

      rut_property_set_binding_by_name (data->editor_camera_component,
                                        "viewport_y",
                                        camera_viewport_binding_cb,
                                        data,
                                        /* XXX: Hack: we are currently relying on
                                         * the bevel width being redundantly re-set
                                         * at times when bevel's position may have
                                         * also changed.
                                         *
                                         * FIXME: We need a proper allocation cycle
                                         * in Rut!
                                         */
                                        main_area_width,
                                        NULL);

      rut_property_set_binding_by_name (data->editor_camera_component,
                                        "viewport_width",
                                        camera_viewport_binding_cb,
                                        data,
                                        main_area_width,
                                        NULL);

      rut_property_set_binding_by_name (data->editor_camera_component,
                                        "viewport_height",
                                        camera_viewport_binding_cb,
                                        data,
                                        main_area_height,
                                        NULL);
    }
  else
#endif /* RIG_EDITOR_ENABLED */
    {
      int width = cogl_framebuffer_get_width (fb);
      int height = cogl_framebuffer_get_height (fb);

      rut_camera_set_viewport (data->editor_camera_component,
                               0, 0, width, height);

      rut_input_region_set_rectangle (data->editor_input_region,
                                      0, 0, width, height);

    }

  /* tool */
  data->tool = rut_tool_new (data->shell);
  rut_tool_add_rotation_event_callback (data->tool,
                                        tool_rotation_event_cb,
                                        data,
                                        NULL /* destroy_cb */);
  rut_tool_set_camera (data->tool, data->editor_camera);

  /* picking ray */
  data->picking_ray_color = cogl_pipeline_new (data->ctx->cogl_context);
  cogl_pipeline_set_color4f (data->picking_ray_color, 1.0, 0.0, 0.0, 1.0);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    set_play_mode_enabled (data, FALSE);
  else
#endif
    set_play_mode_enabled (data, TRUE);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    rig_load_asset_list (data);

  if (data->transitions)
    data->selected_transition = data->transitions->data;
  else
    {
      RigTransition *transition = rig_create_transition (data, 0);
      data->transitions = g_list_prepend (data->transitions, transition);
      data->selected_transition = transition;
    }

  if (!_rig_in_device_mode &&
      data->selected_transition)
    {
      RutObject *doc_node =
        rut_ui_viewport_get_doc_node (data->timeline_vp);

      data->transition_view =
        rig_transition_view_new (data->ctx,
                                 data->scene,
                                 data->selected_transition,
                                 data->timeline,
                                 data->undo_journal);
      rut_graphable_add_child (doc_node, data->transition_view);
      rut_ui_viewport_set_sync_widget (data->timeline_vp,
                                       data->transition_view);
    }
#endif

  allocate (data);

  rig_renderer_init (data);
}

static void
fini (RutShell *shell, void *user_data)
{
  RigData *data = user_data;
  int i;

  rig_renderer_fini (data);

  rut_refable_unref (data->camera);
  rut_refable_unref (data->root);

  for (i = 0; i < RIG_DATA_N_PROPS; i++)
    rut_property_destroy (&data->properties[i]);

  cogl_object_unref (data->circle_texture);

  cogl_object_unref (data->circle_node_attribute);

  rut_dof_effect_free (data->dof);

  rut_tool_free (data->tool);

#ifdef RIG_EDITOR_ENABLED
  if (!_rig_in_device_mode)
    {
      rut_refable_unref (data->timeline_vp);
      rut_refable_unref (data->transition_view);
      cogl_object_unref (data->grid_prim);

      if (data->transparency_grid)
        rut_refable_unref (data->transparency_grid);
    }
#endif
}

static RutInputEventStatus
shell_input_handler (RutInputEvent *event, void *user_data)
{
  RigData *data = user_data;

#if 0
  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      /* Anything that can claim the keyboard focus will do so during
       * motion events so we clear it before running other input
       * callbacks */
      data->key_focus_callback = NULL;
    }
#endif

  switch (rut_input_event_get_type (event))
    {
    case RUT_INPUT_EVENT_TYPE_MOTION:
#if 0
      switch (rut_motion_event_get_action (event))
        {
        case RUT_MOTION_EVENT_ACTION_DOWN:
          //g_print ("Press Down\n");
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        case RUT_MOTION_EVENT_ACTION_UP:
          //g_print ("Release\n");
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        case RUT_MOTION_EVENT_ACTION_MOVE:
          //g_print ("Move\n");
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
#endif
      break;

    case RUT_INPUT_EVENT_TYPE_KEY:
#ifdef RIG_EDITOR_ENABLED
      if (!_rig_in_device_mode &&
          rut_key_event_get_action (event) == RUT_KEY_EVENT_ACTION_DOWN)
        {
          switch (rut_key_event_get_keysym (event))
            {
            case RUT_KEY_s:
              if ((rut_key_event_get_modifier_state (event) &
                   RUT_MODIFIER_CTRL_ON))
                {
                  rig_save (data, _rig_ui_filename);
                  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
                }
              break;
            case RUT_KEY_z:
              if ((rut_key_event_get_modifier_state (event) &
                   RUT_MODIFIER_CTRL_ON))
                {
                  rig_undo_journal_undo (data->undo_journal);
                  return RUT_INPUT_EVENT_STATUS_HANDLED;
                }
              break;
            case RUT_KEY_y:
              if ((rut_key_event_get_modifier_state (event) &
                   RUT_MODIFIER_CTRL_ON))
                {
                  rig_undo_journal_redo (data->undo_journal);
                  return RUT_INPUT_EVENT_STATUS_HANDLED;
                }
              break;

            /* HACK: FIXME: provide a handle in the scene for
             * selecting the camera entity */
            case RUT_KEY_c:
              if ((rut_key_event_get_modifier_state (event) &
                   RUT_MODIFIER_CTRL_ON))
                {
                  rig_set_selected_entity (data, data->editor_camera);
                  update_inspector (data);
                  return RUT_INPUT_EVENT_STATUS_HANDLED;
                }
            }
        }
#endif
      break;
 
    case RUT_INPUT_EVENT_TYPE_TEXT:
      break;
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

#if 0
static RutInputEventStatus
add_light_cb (RutInputRegion *region,
              RutInputEvent *event,
              void *user_data)
{
  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_MOTION)
    {
      if (rut_motion_event_get_action (event) == RUT_MOTION_EVENT_ACTION_DOWN)
        {
          g_print ("Add light!\n");
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}
#endif

RutAsset *
rig_load_asset (RigData *data, GFileInfo *info, GFile *asset_file)
{
  GFile *assets_dir = g_file_new_for_path (data->ctx->assets_location);
  GFile *dir = g_file_get_parent (asset_file);
  char *path = g_file_get_relative_path (assets_dir, asset_file);
  GList *inferred_tags = NULL;
  RutAsset *asset = NULL;

  inferred_tags = rut_infer_asset_tags (data->ctx, info, asset_file);

  if (rut_util_find_tag (inferred_tags, "normal-maps"))
    asset = rut_asset_new_normal_map (data->ctx, path);
  else if (rut_util_find_tag (inferred_tags, "alpha-masks"))
    asset = rut_asset_new_alpha_mask (data->ctx, path);
  else if (rut_util_find_tag (inferred_tags, "image"))
    asset = rut_asset_new_texture (data->ctx, path);
  else if (rut_util_find_tag (inferred_tags, "ply"))
    asset = rut_asset_new_ply_model (data->ctx, path);

  if (asset)
    rut_asset_set_inferred_tags (asset, inferred_tags);

  g_list_free (inferred_tags);

  g_object_unref (assets_dir);
  g_object_unref (dir);
  g_free (path);

  return asset;
}

static void
add_asset (RigData *data, GFileInfo *info, GFile *asset_file)
{
  GFile *assets_dir = g_file_new_for_path (data->ctx->assets_location);
  GFile *dir = g_file_get_parent (asset_file);
  char *path = g_file_get_relative_path (assets_dir, asset_file);
  GList *l;
  RutAsset *asset;

  /* Avoid loading duplicate assets... */
  for (l = data->assets; l; l = l->next)
    {
      RutAsset *existing = l->data;

      if (strcmp (rut_asset_get_path (existing), path) == 0)
        return;
    }

  asset = rig_load_asset (data, info, asset_file);
  if (asset)
    data->assets = g_list_prepend (data->assets, asset);
}

#if 0
static GList *
copy_tags (GList *tags)
{
  GList *l, *copy = NULL;
  for (l = tags; l; l = l->next)
    {
      char *tag = g_intern_string (l->data);
      copy = g_list_prepend (copy, tag);
    }
  return copy;
}
#endif

static void
enumerate_dir_for_assets (RigData *data,
                          GFile *directory);

void
enumerate_file_info (RigData *data, GFile *parent, GFileInfo *info)
{
  GFileType type = g_file_info_get_file_type (info);
  const char *name = g_file_info_get_name (info);

  if (name[0] == '.')
    return;

  if (type == G_FILE_TYPE_DIRECTORY)
    {
      GFile *directory = g_file_get_child (parent, name);

      enumerate_dir_for_assets (data, directory);

      g_object_unref (directory);
    }
  else if (type == G_FILE_TYPE_REGULAR ||
           type == G_FILE_TYPE_SYMBOLIC_LINK)
    {
      if (rut_file_info_is_asset (info, name))
        {
          GFile *image = g_file_get_child (parent, name);
          add_asset (data, info, image);
          g_object_unref (image);
        }
    }
}

#ifdef USE_ASYNC_IO
typedef struct _AssetEnumeratorState
{
  RigData *data;
  GFile *directory;
  GFileEnumerator *enumerator;
  GCancellable *cancellable;
  GList *tags;
} AssetEnumeratorState;

static void
cleanup_assets_enumerator (AssetEnumeratorState *state)
{
  if (state->enumerator)
    g_object_unref (state->enumerator);

  g_object_unref (state->cancellable);
  g_object_unref (state->directory);
  g_list_free (state->tags);

  state->data->asset_enumerators =
    g_list_remove (state->data->asset_enumerators, state);

  g_slice_free (AssetEnumeratorState, state);
}

static void
assets_found_cb (GObject *source_object,
                 GAsyncResult *res,
                 gpointer user_data)
{
  AssetEnumeratorState *state = user_data;
  GList *infos;
  GList *l;

  infos = g_file_enumerator_next_files_finish (state->enumerator,
                                               res,
                                               NULL);
  if (!infos)
    {
      cleanup_assets_enumerator (state);
      return;
    }

  for (l = infos; l; l = l->next)
    enumerate_file_info (state->data, state->directory, l->data);

  g_list_free (infos);

  g_file_enumerator_next_files_async (state->enumerator,
                                      5, /* what's a good number here? */
                                      G_PRIORITY_DEFAULT,
                                      state->cancellable,
                                      asset_found_cb,
                                      state);
}

static void
assets_enumerator_cb (GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
{
  AssetEnumeratorState *state = user_data;
  GError *error = NULL;

  state->enumerator =
    g_file_enumerate_children_finish (state->directory, res, &error);
  if (!state->enumerator)
    {
      g_warning ("Error while looking for assets: %s", error->message);
      g_error_free (error);
      cleanup_assets_enumerator (state);
      return;
    }

  g_file_enumerator_next_files_async (state->enumerator,
                                      5, /* what's a good number here? */
                                      G_PRIORITY_DEFAULT,
                                      state->cancellable,
                                      assets_found_cb,
                                      state);
}

static void
enumerate_dir_for_assets_async (RigData *data,
                                GFile *directory)
{
  AssetEnumeratorState *state = g_slice_new0 (AssetEnumeratorState);

  state->data = data;
  state->directory = g_object_ref (directory);

  state->cancellable = g_cancellable_new ();

  /* NB: we can only use asynchronous IO if we are running with a Glib
   * mainloop */
  g_file_enumerate_children_async (file,
                                   "standard::*",
                                   G_FILE_QUERY_INFO_NONE,
                                   G_PRIORITY_DEFAULT,
                                   state->cancellable,
                                   assets_enumerator_cb,
                                   data);

  data->asset_enumerators = g_list_prepend (data->asset_enumerators, state);
}

#else /* USE_ASYNC_IO */

static void
enumerate_dir_for_assets (RigData *data,
                          GFile *file)
{
  GFileEnumerator *enumerator;
  GError *error = NULL;
  GFileInfo *file_info;

  enumerator = g_file_enumerate_children (file,
                                          "standard::*",
                                          G_FILE_QUERY_INFO_NONE,
                                          NULL,
                                          &error);
  if (!enumerator)
    {
      char *path = g_file_get_path (file);
      g_warning ("Failed to enumerator assets dir %s: %s",
                 path, error->message);
      g_free (path);
      g_error_free (error);
      return;
    }

  while ((file_info = g_file_enumerator_next_file (enumerator,
                                                   NULL,
                                                   &error)))
    {
      enumerate_file_info (data, file, file_info);
    }

  g_object_unref (enumerator);
}
#endif /* USE_ASYNC_IO */

static void
rig_load_asset_list (RigData *data)
{
  GFile *assets_dir = g_file_new_for_path (data->ctx->assets_location);
  RutAsset *asset;

  enumerate_dir_for_assets (data, assets_dir);

  data->diamond_builtin_asset = rut_asset_new_builtin (data->ctx, "diamond.png");
  rut_asset_add_inferred_tag (data->diamond_builtin_asset, "diamond");
  rut_asset_add_inferred_tag (data->diamond_builtin_asset, "builtin");
  rut_asset_add_inferred_tag (data->diamond_builtin_asset, "geom");
  rut_asset_add_inferred_tag (data->diamond_builtin_asset, "geometry");
  data->assets = g_list_prepend (data->assets, data->diamond_builtin_asset);

  data->circle_builtin_asset = rut_asset_new_builtin (data->ctx, "circle.png");
  rut_asset_add_inferred_tag (data->circle_builtin_asset, "shape");
  rut_asset_add_inferred_tag (data->circle_builtin_asset, "circle");
  rut_asset_add_inferred_tag (data->circle_builtin_asset, "builtin");
  rut_asset_add_inferred_tag (data->circle_builtin_asset, "geom");
  rut_asset_add_inferred_tag (data->circle_builtin_asset, "geometry");
  data->assets = g_list_prepend (data->assets, data->circle_builtin_asset);

  data->text_builtin_asset = rut_asset_new_builtin (data->ctx, "fonts.png");
  rut_asset_add_inferred_tag (data->text_builtin_asset, "text");
  rut_asset_add_inferred_tag (data->text_builtin_asset, "label");
  rut_asset_add_inferred_tag (data->text_builtin_asset, "builtin");
  rut_asset_add_inferred_tag (data->text_builtin_asset, "geom");
  rut_asset_add_inferred_tag (data->text_builtin_asset, "geometry");
  data->assets = g_list_prepend (data->assets, data->text_builtin_asset);

  g_object_unref (assets_dir);

  rig_search_asset_list (data, NULL);
}

void
rig_free_ux (RigData *data)
{
  GList *l;

  for (l = data->transitions; l; l = l->next)
    rig_transition_free (l->data);
  g_list_free (data->transitions);
  data->transitions = NULL;

  for (l = data->assets; l; l = l->next)
    rut_refable_unref (l->data);
  g_list_free (data->assets);
  data->assets = NULL;

  free_asset_input_closures (data);
}

static void
init_types (void)
{
}

#ifdef __ANDROID__

void
android_main (struct android_app *application)
{
  RigData data;

  /* Make sure glue isn't stripped */
  app_dummy ();

  g_android_init ();

  memset (&data, 0, sizeof (RigData));
  data.app = application;

  init_types ();

  data.shell = rut_android_shell_new (application,
                                      init,
                                      fini,
                                      paint,
                                      &data);

  data.ctx = rut_context_new (data.shell);

  rut_context_init (data.ctx);

  rut_shell_set_input_callback (data.shell, shell_input_handler, &data);

  rut_shell_main (data.shell);
}

#else

int
main (int argc, char **argv)
{
  RigData data;
  GOptionContext *context = g_option_context_new (NULL);
  char *assets_location;
  GError *error = NULL;

  g_option_context_add_main_entries (context, rut_handset_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      fprintf (stderr, "option parsing failed: %s\n", error->message);
      exit (EXIT_FAILURE);
    }

  if (_rig_handset_remaining_args == NULL ||
      _rig_handset_remaining_args[0] == NULL)
    {
      fprintf (stderr,
               "A filename argument for the UI description file is required. "
               "Pass a non-existing file to create it.\n");
      exit (EXIT_FAILURE);
    }

  _rig_ui_filename = _rig_handset_remaining_args[0];

  memset (&data, 0, sizeof (RigData));

  init_types ();

  data.shell = rut_shell_new (init, fini, paint, &data);

  data.ctx = rut_context_new (data.shell);

  assets_location = g_path_get_dirname (_rig_ui_filename);
  rut_set_assets_location (data.ctx, assets_location);
  g_free (assets_location);

  rut_context_init (data.ctx);

  rut_shell_add_input_callback (data.shell, shell_input_handler, &data, NULL);

  rut_shell_main (data.shell);

  return 0;
}

#endif
