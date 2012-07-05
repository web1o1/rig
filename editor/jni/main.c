/*
 * Rig
 *
 * Copyright (C) 2012  Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <math.h>

#include <rig.h>

#define N_CUBES 10

typedef struct _RigTool
{
  RigShell *shell;
  RigEntity *selected_entity;
  RigEntity *rotation_tool;
  RigEntity *rotation_tool_handle;
  RigInputRegion *rotation_circle;
  RigArcball arcball;
  CoglQuaternion saved_rotation;
  bool button_down;
  RigEntity *camera;
  RigCamera *camera_component; /* cameracomponent of the camera above */
  float position[3];    /* transformed position of the selected entity */
  float screen_pos[2];
  float scale;
} RigTool;

typedef struct
{
  RigShell *shell;
  RigContext *ctx;

  CoglFramebuffer *fb;
  float fb_width, fb_height;
  GTimer *timer;

  /* postprocessing */
  CoglFramebuffer *postprocess;
  CoglTexture2D *postprocess_color;
  CoglPipeline *pp_pipeline;

  /* scene */
  RigGraph *scene;
  RigEntity *main_camera;
  RigCamera *main_camera_component;
  float main_camera_z;
  RigEntity *light;
  RigEntity *ui_camera;
  RigCamera *ui_camera_component;
  RigEntity *plane;
  RigEntity *cubes[N_CUBES];
  GList *entities;
  GList *pickables;

  /* shadow mapping */
  CoglOffscreen *shadow_fb;
  CoglTexture2D *shadow_color;
  CoglTexture   *shadow_map;
  RigCamera     *shadow_map_camera;

  CoglPipeline *shadow_color_tex;
  CoglPipeline *shadow_map_tex;

  /* root materials */
  CoglPipeline *diffuse_specular;

  /* editor state */
  bool button_down;
  RigArcball arcball;
  CoglQuaternion saved_rotation;
  RigEntity *selected_entity;
  RigTool *tool;
  bool edit;      /* in edit mode, we can temper with the entities. When edit
                     is turned off, we'll do the full render (including post
                     processing) as post-processing does interact well with
                     drawing the tools */

  /* picking ray */
  CoglPipeline *picking_ray_color;
  CoglPrimitive *picking_ray;

  /* debug features */
  bool debug_pick_ray;
  bool debug_shadows;

} Data;

/*
 * Materials
 */

static CoglPipeline *
create_color_pipeline (float r, float g, float b)
{
  static CoglPipeline *template = NULL, *new_pipeline;

  if (G_UNLIKELY (template == NULL))
    template = cogl_pipeline_new (rig_cogl_context);

  new_pipeline = cogl_pipeline_copy (template);
  cogl_pipeline_set_color4f (new_pipeline, r, g, b, 1.0f);

  return new_pipeline;
}

void
rig_entity_apply_rotations (RigObject *entity,
                            CoglQuaternion *rotations)
{
  int depth = 0;
  RigObject **entity_nodes;
  RigObject *node = entity;
  int i;

  do {
    RigGraphableProps *graphable_priv =
      rig_object_get_properties (node, RIG_INTERFACE_ID_GRAPHABLE);

    depth++;

    node = graphable_priv->parent;
  } while (node);

  entity_nodes = g_alloca (sizeof (RigObject *) * depth);

  node = entity;
  i = 0;
  do {
    RigGraphableProps *graphable_priv;
    RigObjectProps *obj = node;

    if (obj->type == &rig_entity_type)
      entity_nodes[i++] = node;

    graphable_priv =
      rig_object_get_properties (node, RIG_INTERFACE_ID_GRAPHABLE);
    node = graphable_priv->parent;
  } while (node);

  for (i--; i >= 0; i--)
    {
      const CoglQuaternion *rotation = rig_entity_get_rotation (entity_nodes[i]);
      cogl_quaternion_multiply (rotations, rotations, rotation);
    }
}

void
rig_entity_get_rotations (RigObject *entity,
                          CoglQuaternion *rotation)
{
  cogl_quaternion_init_identity (rotation);
  rig_entity_apply_rotations (entity, rotation);
}

void
rig_entity_get_view_rotations (RigObject *entity,
                               RigObject *camera_entity,
                               CoglQuaternion *rotation)
{
  rig_entity_get_rotations (camera_entity, rotation);
  cogl_quaternion_invert (rotation);

  rig_entity_apply_rotations (entity, rotation);
}

/*
 * RigTool
 */

static RigInputEventStatus
on_rotation_tool_clicked (RigInputRegion *region,
                          RigInputEvent *event,
                          void *user_data)
{
  RigTool *tool = user_data;
  RigMotionEventAction action;
  RigButtonState state;
  RigInputEventStatus status = RIG_INPUT_EVENT_STATUS_UNHANDLED;
  RigEntity *entity;
  float x, y;

  entity = tool->selected_entity;

  switch (rig_input_event_get_type (event))
    {
      case RIG_INPUT_EVENT_TYPE_MOTION:
      {
        action = rig_motion_event_get_action (event);
        state = rig_motion_event_get_button_state (event);
        x = rig_motion_event_get_x (event);
        y = rig_motion_event_get_y (event);

        y = -y + 2 * (tool->screen_pos[1]);

        if (action == RIG_MOTION_EVENT_ACTION_DOWN &&
            state == RIG_BUTTON_STATE_1)
          {
            rig_input_region_set_circle (tool->rotation_circle,
                                         tool->screen_pos[0],
                                         tool->screen_pos[1],
                                         128);


            rig_arcball_init (&tool->arcball,
                              tool->screen_pos[0],
                              tool->screen_pos[1],
                              128);

            rig_entity_get_view_rotations (entity, tool->camera, &tool->saved_rotation);

            cogl_quaternion_init_identity (&tool->arcball.q_drag);

            rig_arcball_mouse_down (&tool->arcball, x, y);

            tool->button_down = TRUE;

            status = RIG_INPUT_EVENT_STATUS_HANDLED;
          }
        else if (action == RIG_MOTION_EVENT_ACTION_MOVE &&
                 state == RIG_BUTTON_STATE_1)
          {
            CoglQuaternion camera_rotation;
            CoglQuaternion new_rotation;
            RigEntity *parent;
            CoglQuaternion parent_inverse;

            if (!tool->button_down)
              break;

            rig_arcball_mouse_motion (&tool->arcball, x, y);

            cogl_quaternion_multiply (&camera_rotation,
                                      &tool->arcball.q_drag,
                                      &tool->saved_rotation);

            /* XXX: We have calculated the combined rotation in camera
             * space, we now need to separate out the rotation of the
             * entity itself.
             *
             * We rotate by the inverse of the parent's view transform
             * so we are left with just the entity's rotation.
             */
            parent = rig_graphable_get_parent (entity);

            rig_entity_get_view_rotations (parent,
                                           tool->camera,
                                           &parent_inverse);
            cogl_quaternion_invert (&parent_inverse);

            cogl_quaternion_multiply (&new_rotation,
                                      &parent_inverse,
                                      &camera_rotation);

            rig_entity_set_rotation (entity, &new_rotation);

            status = RIG_INPUT_EVENT_STATUS_HANDLED;
          }
        else if (action == RIG_MOTION_EVENT_ACTION_UP)
          {
            tool->button_down = FALSE;

            rig_input_region_set_circle (tool->rotation_circle, x, y, 64);
          }

        break;
      }

      case RIG_INPUT_EVENT_TYPE_KEY:
        break;
    }

  return status;
}

static RigTool *
rig_tool_new (Data *data)
{
  RigTool *tool;
  RigObject *component;
  CoglPipeline *pipeline;

  tool = g_slice_new0 (RigTool);

  tool->shell = data->shell;

  /* rotation tool */
  tool->rotation_tool = rig_entity_new (data->ctx);

  pipeline = create_color_pipeline (1.f, 1.f, 1.f);
  component = rig_mesh_renderer_new_from_template ("rotation-tool");
  rig_entity_add_component (tool->rotation_tool, component);
  component = rig_material_new_with_pipeline (data->ctx, pipeline);
  rig_entity_add_component (tool->rotation_tool, component);

  /* rotation tool handle circle */
  tool->rotation_tool_handle = rig_entity_new (data->ctx);
  component = rig_mesh_renderer_new_from_template ("circle");
  rig_entity_add_component (tool->rotation_tool_handle, component);
  component = rig_material_new_with_pipeline (data->ctx, pipeline);
  rig_entity_add_component (tool->rotation_tool_handle, component);

  cogl_object_unref (pipeline);

  tool->rotation_circle =
    rig_input_region_new_circle (0, 0, 0, on_rotation_tool_clicked, tool);

  return tool;
}

static void
rig_tool_set_camera (RigTool *tool,
                     RigEntity *camera)
{
  tool->camera = camera;
}

void
get_modelview_matrix (RigEntity  *camera,
                      RigEntity  *entity,
                      CoglMatrix *modelview)
{
  RigCamera *camera_component =
    rig_entity_get_component (camera, RIG_COMPONENT_TYPE_CAMERA);
  *modelview = *rig_camera_get_view_transform (camera_component);

  cogl_matrix_multiply (modelview,
                        modelview,
                        rig_entity_get_transform (entity));
}

/* Scale from OpenGL normalized device coordinates (ranging from -1 to 1)
 * to Cogl window/framebuffer coordinates (ranging from 0 to buffer-size) with
 * (0,0) being top left. */
#define VIEWPORT_TRANSFORM_X(x, vp_origin_x, vp_width) \
    (  ( ((x) + 1.0) * ((vp_width) / 2.0) ) + (vp_origin_x)  )
/* Note: for Y we first flip all coordinates around the X axis while in
 * normalized device coodinates */
#define VIEWPORT_TRANSFORM_Y(y, vp_origin_y, vp_height) \
    (  ( ((-(y)) + 1.0) * ((vp_height) / 2.0) ) + (vp_origin_y)  )

/* to call every time the selected entity changes or when the one already
 * selected changes transform. As we have now way to be notified if the
 * transform of an entity has change (yet!) this is called every frame
 * before drawing the tool */
static void
rig_tool_update (RigTool *tool,
                 RigEntity *selected_entity)
{
    RigComponent *camera;
    CoglMatrix transform;
    const CoglMatrix *projection;
    float scale_thingy[4], screen_space[4], x, y;
    const float *viewport;

    if (selected_entity == NULL)
      {
        tool->selected_entity = NULL;

        /* remove the input region when no entity is selected */
        rig_shell_remove_input_region (tool->shell, tool->rotation_circle);

        return;
      }

    /* transform the selected entity up to the projection */
    get_modelview_matrix (tool->camera,
                          selected_entity,
                          &transform);

    tool->position[0] = tool->position[1] = tool->position[2] = 0.f;

    cogl_matrix_transform_points (&transform,
                                  3, /* num components for input */
                                  sizeof (float) * 3, /* input stride */
                                  tool->position,
                                  sizeof (float) * 3, /* output stride */
                                  tool->position,
                                  1 /* n_points */);

    camera = rig_entity_get_component (tool->camera,
                                       RIG_COMPONENT_TYPE_CAMERA);
    projection = rig_camera_get_projection (RIG_CAMERA (camera));

    scale_thingy[0] = 1.f;
    scale_thingy[1] = 0.f;
    scale_thingy[2] = tool->position[2];

    cogl_matrix_project_points (projection,
                                3, /* num components for input */
                                sizeof (float) * 3, /* input stride */
                                scale_thingy,
                                sizeof (float) * 4, /* output stride */
                                scale_thingy,
                                1 /* n_points */);
    scale_thingy[0] /= scale_thingy[3];

    tool->scale = 1. / scale_thingy[0];

    /* update the input region, need project the transformed point and do
     * the viewport transform */
    screen_space[0] = tool->position[0];
    screen_space[1] = tool->position[1];
    screen_space[2] = tool->position[2];
    cogl_matrix_project_points (projection,
                                3, /* num components for input */
                                sizeof (float) * 3, /* input stride */
                                screen_space,
                                sizeof (float) * 4, /* output stride */
                                screen_space,
                                1 /* n_points */);

    /* perspective divide */
    screen_space[0] /= screen_space[3];
    screen_space[1] /= screen_space[3];

    /* apply viewport transform */
    viewport = rig_camera_get_viewport (RIG_CAMERA (camera));
    x = VIEWPORT_TRANSFORM_X (screen_space[0], viewport[0], viewport[2]);
    y = VIEWPORT_TRANSFORM_Y (screen_space[1], viewport[1], viewport[3]);

    tool->screen_pos[0] = x;
    tool->screen_pos[1] = y;

    if (!tool->button_down)
      rig_input_region_set_circle (tool->rotation_circle, x, y, 64);

    if (tool->selected_entity != selected_entity)
      {
        /* If we go from a "no entity selected" state to a "entity selected"
         * one, we set-up the input region */
        if (tool->selected_entity == NULL)
            rig_shell_add_input_region (tool->shell, tool->rotation_circle);

        tool->selected_entity = selected_entity;
      }

    /* save the camera component for other functions to use */
    tool->camera_component = RIG_CAMERA (camera);
}

static float
rig_tool_get_scale_for_length (RigTool *tool,
                               float    length)
{
  return length * tool->scale;
}

static void
get_rotation (Data       *data,
              RigEntity  *camera,
              RigEntity  *entity,
              CoglMatrix *rotation)
{
  CoglQuaternion q;
  rig_entity_get_view_rotations (entity, camera, &q);
  cogl_matrix_init_from_quaternion (rotation, &q);
}

static void
rig_tool_draw (RigTool *tool,
               Data *data, /* same story as _update() */
               CoglFramebuffer *fb)
{
  CoglMatrix rotation;
  float scale, aspect_ratio;
  CoglMatrix saved_projection;

  float fb_width, fb_height;

  get_rotation (data,
                tool->camera,
                tool->selected_entity,
                &rotation);

  /* we change the projection matrix to clip at -position[2] to clip the
   * half sphere that is away from the camera */
  fb_width = cogl_framebuffer_get_width (fb);
  fb_height = cogl_framebuffer_get_height (fb);
  aspect_ratio = fb_width / fb_height;

  cogl_framebuffer_get_projection_matrix (fb, &saved_projection);
  cogl_framebuffer_perspective (fb,
                                rig_camera_get_field_of_view (tool->camera_component),
                                aspect_ratio,
                                rig_camera_get_near_plane (tool->camera_component),
                                -tool->position[2]);

  scale = rig_tool_get_scale_for_length (tool, 128 / fb_width);

  /* draw the tool */
  cogl_framebuffer_push_matrix (fb);
  cogl_framebuffer_identity_matrix (fb);
  cogl_framebuffer_translate (fb,
                              tool->position[0],
                              tool->position[1],
                              tool->position[2]);
  cogl_framebuffer_scale (fb, scale, scale, scale);
  cogl_framebuffer_push_matrix (fb);
  cogl_framebuffer_transform (fb, &rotation);
  rig_entity_draw (tool->rotation_tool, fb);
  cogl_framebuffer_pop_matrix (fb);
  rig_entity_draw (tool->rotation_tool_handle, fb);
  cogl_framebuffer_scale (fb, 1.1, 1.1, 1.1);
  rig_entity_draw (tool->rotation_tool_handle, fb);
  cogl_framebuffer_pop_matrix (fb);

  cogl_framebuffer_set_projection_matrix (fb, &saved_projection);
}

/* in micro seconds  */
static int64_t
get_current_time (Data *data)
{
  return (int64_t) (g_timer_elapsed (data->timer, NULL) * 1e6);
}

static void
compute_light_shadow_matrix (Data       *data,
                             CoglMatrix *light_matrix,
                             CoglMatrix *light_projection,
                             RigEntity  *light)
{
  CoglMatrix *main_camera, *light_transform, light_view;
  /* Move the unit data from [-1,1] to [0,1], column major order */
  float bias[16] = {
    .5f, .0f, .0f, .0f,
    .0f, .5f, .0f, .0f,
    .0f, .0f, .5f, .0f,
    .5f, .5f, .5f, 1.f
  };

  main_camera = rig_entity_get_transform (data->main_camera);
  light_transform = rig_entity_get_transform (light);
  cogl_matrix_get_inverse (light_transform, &light_view);

  cogl_matrix_init_from_array (light_matrix, bias);
  cogl_matrix_multiply (light_matrix, light_matrix, light_projection);
  cogl_matrix_multiply (light_matrix, light_matrix, &light_view);
  cogl_matrix_multiply (light_matrix, light_matrix, main_camera);
}

CoglPipeline *
create_diffuse_specular_material (void)
{
  CoglPipeline *pipeline;
  CoglSnippet *snippet;
  CoglDepthState depth_state;

  pipeline = cogl_pipeline_new (rig_cogl_context);
  cogl_pipeline_set_color4f (pipeline, 0.8f, 0.8f, 0.8f, 1.f);

  /* enable depth testing */
  cogl_depth_state_init (&depth_state);
  cogl_depth_state_set_test_enabled (&depth_state, TRUE);
  cogl_pipeline_set_depth_state (pipeline, &depth_state, NULL);

  /* set up our vertex shader */
  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_VERTEX,

      /* definitions */
      "uniform mat4 light_shadow_matrix;\n"
      "uniform mat3 normal_matrix;\n"
      "varying vec3 normal_direction, eye_direction;\n"
      "varying vec4 shadow_coords;\n",

      "normal_direction = normalize(normal_matrix * cogl_normal_in);\n"
      "eye_direction    = -vec3(cogl_modelview_matrix * cogl_position_in);\n"

      "shadow_coords = light_shadow_matrix * cogl_modelview_matrix *\n"
      "                cogl_position_in;\n"
  );

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  /* and fragment shader */
  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
      /* definitions */
      "uniform vec4 light0_ambient, light0_diffuse, light0_specular;\n"
      "uniform vec3 light0_direction_norm;\n"
      "varying vec3 normal_direction, eye_direction;\n",

      /* post */
      NULL);

  cogl_snippet_set_replace (snippet,
      "vec4 final_color = light0_ambient * cogl_color_in;\n"

      " vec3 L = light0_direction_norm;\n"
      " vec3 N = normalize(normal_direction);\n"

      "float lambert = dot(N, L);\n"

      "if (lambert > 0.0)\n"
      "{\n"
      "  final_color += cogl_color_in * light0_diffuse * lambert;\n"

      "  vec3 E = normalize(eye_direction);\n"
      "  vec3 R = reflect (-L, N);\n"
      "  float specular = pow (max(dot(R, E), 0.0),\n"
      "                        2.);\n"
      "  final_color += light0_specular * vec4(.6, .6, .6, 1.0) * specular;\n"
      "}\n"

      "shadow_coords_d = shadow_coords / shadow_coords.w;\n"
      "cogl_texel7 =  cogl_texture_lookup7 (cogl_sampler7, cogl_tex_coord_in[0]);\n"
      "float distance_from_light = cogl_texel7.z + 0.0005;\n"
      "float shadow = 1.0;\n"
      "if (shadow_coords.w > 0.0 && distance_from_light < shadow_coords_d.z)\n"
      "    shadow = 0.5;\n"

      "cogl_color_out = shadow * final_color;\n"
  );

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  return pipeline;
}

static CoglPipeline *
create_pp_pipeline (CoglTexture *texture)
{
  CoglPipeline *new_pipeline;

  new_pipeline = cogl_pipeline_new (rig_cogl_context);
  cogl_pipeline_set_layer_texture (new_pipeline, 0, texture);

  return new_pipeline;
}

static void
draw_entities (Data            *data,
               CoglFramebuffer *fb,
               RigEntity       *camera,
               gboolean         shadow_pass)
{
  GList *l;

  for (l = data->entities; l; l = l->next)
    {
      RigEntity *entity = l->data;
      const CoglMatrix *transform;

      if (shadow_pass && !rig_entity_get_cast_shadow (entity))
        continue;

      cogl_framebuffer_push_matrix (fb);

      transform = rig_entity_get_transform (entity);
      cogl_framebuffer_transform (fb, transform);

      rig_entity_draw (entity, fb);

      cogl_framebuffer_pop_matrix (fb);
    }
}

static void
test_init (RigShell *shell, void *user_data)
{
  Data *data = user_data;
  CoglOnscreen *onscreen;
  CoglTexture2D *color_buffer;
  GError *error = NULL;
  RigObject *component;
  CoglPipeline *root_pipeline, *pipeline;
  CoglSnippet *snippet;
  CoglColor color;
  float vector3[3];
  int i;

  data->ctx = rig_context_new (data->shell);

  rig_context_init (data->ctx);

  data->fb_width = 800;
  data->fb_height = 600;
  onscreen = cogl_onscreen_new (data->ctx->cogl_context,
                                data->fb_width,
                                data->fb_height);
  data->fb = COGL_FRAMEBUFFER (onscreen);

  cogl_onscreen_show (onscreen);

  /*
   * Offscreen render for post-processing
   */
  color_buffer = cogl_texture_2d_new_with_size (rig_cogl_context,
                                                data->fb_width,
                                                data->fb_height,
                                                COGL_PIXEL_FORMAT_RGBA_8888,
                                                &error);
  if (error)
    g_critical ("could not create texture: %s", error->message);

  data->postprocess = COGL_FRAMEBUFFER (
    cogl_offscreen_new_to_texture (COGL_TEXTURE (color_buffer)));

  data->postprocess_color = color_buffer;

  /* postprocessing pipeline */
  data->pp_pipeline = create_pp_pipeline (COGL_TEXTURE (color_buffer));

  /*
   * Shadow mapping
   */

  /* Setup the shadow map */
  color_buffer = cogl_texture_2d_new_with_size (rig_cogl_context,
                                                512, 512,
                                                COGL_PIXEL_FORMAT_ANY,
                                                &error);
  if (error)
    g_critical ("could not create texture: %s", error->message);

  data->shadow_color = color_buffer;

  /* XXX: Right now there's no way to disable rendering to the the color
   * buffer. */
  data->shadow_fb =
    cogl_offscreen_new_to_texture (COGL_TEXTURE (color_buffer));

  /* retrieve the depth texture */
  cogl_framebuffer_enable_depth_texture (COGL_FRAMEBUFFER (data->shadow_fb),
                                         TRUE);
  data->shadow_map =
    cogl_framebuffer_get_depth_texture (COGL_FRAMEBUFFER (data->shadow_fb));

  if (data->shadow_fb == NULL)
    g_critical ("could not create offscreen buffer");

  /* Hook the shadow sampling */
  root_pipeline = create_diffuse_specular_material ();
  cogl_pipeline_set_layer_texture (root_pipeline, 7, data->shadow_map);

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_TEXTURE_LOOKUP,
                              /* declarations */
                              "varying vec4 shadow_coords;\n"
                              "vec4 shadow_coords_d;\n",
                              /* post */
                              "");

  cogl_snippet_set_replace (snippet,
                            "cogl_texel = texture2D(cogl_sampler7, shadow_coords_d.st);\n");

  cogl_pipeline_add_layer_snippet (root_pipeline, 7, snippet);
  cogl_object_unref (snippet);

  /*
   * Setup CoglObjects to render our plane and cube
   */

  data->scene = rig_graph_new (data->ctx, NULL);

  /* camera */
  data->main_camera = rig_entity_new (data->ctx);
  data->entities = g_list_prepend (data->entities, data->main_camera);

  data->main_camera_z = 20.f;
  vector3[0] = 0.f;
  vector3[1] = 0.f;
  vector3[2] = data->main_camera_z;
  rig_entity_set_position (data->main_camera, vector3);

  component = rig_camera_new (data->ctx, data->fb);
  data->main_camera_component = component;

  rig_camera_set_projection_mode (RIG_CAMERA (component),
                                  RIG_PROJECTION_PERSPECTIVE);
  rig_camera_set_field_of_view (RIG_CAMERA (component), 60.f);
  rig_camera_set_near_plane (RIG_CAMERA (component), 1.1f);
  rig_camera_set_far_plane (RIG_CAMERA (component), 100.f);

  rig_entity_add_component (data->main_camera, component);

  rig_graphable_add_child (data->scene, data->main_camera);

  /* light */
  data->light = rig_entity_new (data->ctx);
  data->entities = g_list_prepend (data->entities, data->light);

  vector3[0] = 12.0f;
  vector3[1] = 8.0f;
  vector3[2] = -2.0f;
  rig_entity_set_position (data->light, vector3);

  rig_entity_rotate_x_axis (data->light, -120);
  rig_entity_rotate_y_axis (data->light, 10);

  component = rig_light_new ();
  cogl_color_init_from_4f (&color, .2f, .2f, .2f, 1.f);
  rig_light_set_ambient (RIG_LIGHT (component), &color);
  cogl_color_init_from_4f (&color, .6f, .6f, .6f, 1.f);
  rig_light_set_diffuse (RIG_LIGHT (component), &color);
  cogl_color_init_from_4f (&color, .4f, .4f, .4f, 1.f);
  rig_light_set_specular (RIG_LIGHT (component), &color);
  rig_light_add_pipeline (RIG_LIGHT (component), root_pipeline);


  rig_entity_add_component (data->light, component);

  component = rig_camera_new (data->ctx, COGL_FRAMEBUFFER (data->shadow_fb));
  data->shadow_map_camera = component;

  rig_camera_set_background_color4f (RIG_CAMERA (component), 0.f, .3f, 0.f, 1.f);
  rig_camera_set_projection_mode (RIG_CAMERA (component),
                                  RIG_PROJECTION_ORTHOGRAPHIC);
  rig_camera_set_orthographic_coordinates (RIG_CAMERA (component),
                                           15, 5, -15, -5);
  rig_camera_set_near_plane (RIG_CAMERA (component), 1.1f);
  rig_camera_set_far_plane (RIG_CAMERA (component), 20.f);

  rig_entity_add_component (data->light, component);

  rig_graphable_add_child (data->scene, data->light);

  /* plane */
  data->plane = rig_entity_new (data->ctx);
  data->entities = g_list_prepend (data->entities, data->plane);
  data->pickables = g_list_prepend (data->pickables, data->plane);
  rig_entity_set_cast_shadow (data->plane, FALSE);
  rig_entity_set_y (data->plane, -1.5f);

  component = rig_mesh_renderer_new_from_template ("plane");
  rig_entity_add_component (data->plane, component);
  component = rig_material_new_with_pipeline (data->ctx, root_pipeline);
  rig_entity_add_component (data->plane, component);

  rig_graphable_add_child (data->scene, data->plane);

  /* 5 cubes */
  pipeline = cogl_pipeline_copy (root_pipeline);
  cogl_pipeline_set_color4f (pipeline, 0.6f, 0.6f, 0.6f, 1.0f);
  for (i = 0; i < N_CUBES; i++)
    {

      data->cubes[i] = rig_entity_new (data->ctx);
      data->entities = g_list_prepend (data->entities, data->cubes[i]);
      data->pickables = g_list_prepend (data->pickables, data->cubes[i]);

      rig_entity_set_cast_shadow (data->cubes[i], TRUE);
      rig_entity_set_x (data->cubes[i], i * 2.5f);
      rig_entity_set_y (data->cubes[i], .5);
      rig_entity_set_z (data->cubes[i], 1);
      rig_entity_rotate_y_axis (data->cubes[i], 10);

      component = rig_mesh_renderer_new_from_template ("cube");
      rig_entity_add_component (data->cubes[i], component);
      component = rig_material_new_with_pipeline (data->ctx, pipeline);
      cogl_object_unref (pipeline);
      rig_entity_add_component (data->cubes[i], component);

      rig_graphable_add_child (data->scene, data->cubes[i]);
    }

  /* create the pipelines to display the shadow color and depth textures */
  data->shadow_color_tex =
      rig_util_create_texture_pipeline (COGL_TEXTURE (data->shadow_color));
  data->shadow_map_tex =
      rig_util_create_texture_pipeline (COGL_TEXTURE (data->shadow_map));

  cogl_object_unref (root_pipeline);

  /* editor data */
  {
    int w, h;

    w = cogl_framebuffer_get_width (data->fb);
    h = cogl_framebuffer_get_height (data->fb);

    rig_arcball_init (&data->arcball, w / 2, h / 2, sqrtf (w * w + h * h) / 2);

    /* picking ray */
    data->picking_ray_color = create_color_pipeline (1.f, 0.f, 0.f);

  }

  /* UI layer camera */
  data->ui_camera = rig_entity_new (data->ctx);

  component = rig_camera_new (data->ctx, data->fb);
  data->ui_camera_component = component;
  rig_camera_set_projection_mode (RIG_CAMERA (component),
                                  RIG_PROJECTION_ORTHOGRAPHIC);
  rig_camera_set_orthographic_coordinates (RIG_CAMERA (component),
                                           0,
                                           0,
                                           data->fb_width,
                                           data->fb_height);
  rig_camera_set_near_plane (RIG_CAMERA (component), -64.f);
  rig_camera_set_far_plane (RIG_CAMERA (component), 64.f);
  rig_camera_set_clear (RIG_CAMERA (component), FALSE);

  rig_entity_add_component (data->ui_camera, component);

  /* tool */
  data->tool = rig_tool_new (data);
  rig_tool_set_camera (data->tool, data->main_camera);

  /* we default to edit mode */
  data->edit = TRUE;

  /* We draw/pick the entities in the order they are listed and so
   * that matches the order we created the entities we now reverse the
   * list... */
  data->entities = g_list_reverse (data->entities);
  data->pickables = g_list_reverse (data->pickables);

  /* timer for the world time */
  data->timer = g_timer_new ();
  g_timer_start (data->timer);
}

static void
camera_update_view (RigEntity *camera, CoglBool shadow_map)
{
  RigCamera *camera_component =
    rig_entity_get_component (camera, RIG_COMPONENT_TYPE_CAMERA);
  CoglMatrix transform;
  CoglMatrix view;

  rig_graphable_get_transform (camera, &transform);
  cogl_matrix_get_inverse (&transform, &view);

  if (shadow_map)
    {
      CoglMatrix flipped_view;
      cogl_matrix_init_identity (&flipped_view);
      cogl_matrix_scale (&flipped_view, 1, -1, 1);
      cogl_matrix_multiply (&flipped_view, &flipped_view, &view);
      rig_camera_set_view_transform (camera_component, &flipped_view);
    }
  else
    rig_camera_set_view_transform (camera_component, &view);
}

static CoglBool
test_paint (RigShell *shell, void *user_data)
{
  Data *data = user_data;
  int64_t time; /* micro seconds */
  CoglFramebuffer *shadow_fb, *draw_fb;
  GList *l;

  /*
   * update entities
   */

  time = get_current_time (data);

  camera_update_view (data->main_camera, FALSE);
  camera_update_view (data->light, TRUE);
  camera_update_view (data->ui_camera, FALSE);

  for (l = data->entities; l; l = l->next)
    {
      RigEntity *entity = l->data;

      rig_entity_update (entity, time);
    }
  rig_entity_update (data->ui_camera, time);

  /*
   * render the shadow map
   */

  shadow_fb = COGL_FRAMEBUFFER (data->shadow_fb);

  /* update the light matrix uniform */
  {
    CoglMatrix light_shadow_matrix, light_projection;
    CoglPipeline *pipeline;
    RigMaterial *material;

    int location;

    cogl_framebuffer_get_projection_matrix (shadow_fb, &light_projection);
    compute_light_shadow_matrix (data,
                                 &light_shadow_matrix,
                                 &light_projection,
                                 data->light);

    material = rig_entity_get_component (data->plane, RIG_COMPONENT_TYPE_MATERIAL);
    pipeline = rig_material_get_pipeline (material);
    location = cogl_pipeline_get_uniform_location (pipeline,
                                                   "light_shadow_matrix");
    cogl_pipeline_set_uniform_matrix (pipeline,
                                      location,
                                      4, 1,
                                      FALSE,
                                      cogl_matrix_get_array (&light_shadow_matrix));

    material = rig_entity_get_component (data->cubes[0], RIG_COMPONENT_TYPE_MATERIAL);
    pipeline = rig_material_get_pipeline (material);
    location = cogl_pipeline_get_uniform_location (pipeline,
                                                   "light_shadow_matrix");
    cogl_pipeline_set_uniform_matrix (pipeline,
                                      location,
                                      4, 1,
                                      FALSE,
                                      cogl_matrix_get_array (&light_shadow_matrix));
  }

  rig_camera_flush (data->shadow_map_camera);

  draw_entities (data, shadow_fb, data->light, TRUE /* shadow pass */);

  rig_camera_end_frame (data->shadow_map_camera);

  /*
   * render the scene
   */

  /* post processing or not? */
  if (data->edit)
    draw_fb = data->fb;
  else
    draw_fb = data->postprocess;

  rig_camera_set_framebuffer (data->main_camera_component, draw_fb);

  rig_camera_flush (data->main_camera_component);

  /* draw entities */
  draw_entities (data, draw_fb, data->main_camera, FALSE);

  if (data->debug_pick_ray && data->picking_ray)
    {
      cogl_framebuffer_draw_primitive (draw_fb,
                                       data->picking_ray_color,
                                       data->picking_ray);
    }

  if (data->edit && data->selected_entity)
    {
      rig_tool_update (data->tool, data->selected_entity);
      rig_tool_draw (data->tool, data, data->fb);
    }

  rig_camera_end_frame (data->main_camera_component);

  /* The UI layer is drawn using an orthographic projection */
  rig_camera_flush (data->ui_camera_component);

  cogl_framebuffer_push_matrix (data->fb);
  cogl_framebuffer_identity_matrix (data->fb);

  /* draw the postprocess framebuffer to the real onscreen with the
   * postprocess pipeline */
  if (!data->edit)
    {
      cogl_framebuffer_draw_rectangle (data->fb, data->pp_pipeline,
                                       0, 0, data->fb_width, data->fb_height);
    }

  /* draw the color and depth buffers of the shadow FBO to debug them */
  if (data->debug_shadows)
    {
      cogl_framebuffer_draw_rectangle (data->fb, data->shadow_color_tex,
                                       128, 128, 0, 0);
      cogl_framebuffer_draw_rectangle (data->fb, data->shadow_map_tex,
                                       128, 256, 0, 128);
    }

  cogl_framebuffer_pop_matrix (data->fb);

  rig_camera_end_frame (data->ui_camera_component);

  cogl_onscreen_swap_buffers (COGL_ONSCREEN (data->fb));

  return TRUE;
}

static void
test_fini (RigShell *shell, void *user_data)
{

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

  attribute_buffer = cogl_attribute_buffer_new (rig_cogl_context,
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

  rig_util_transform_normal (&normal_matrix,
                             &ray_direction[0],
                             &ray_direction[1],
                             &ray_direction[2]);
}

static CoglPrimitive *
create_picking_ray (Data            *data,
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

static RigEntity *
pick (Data  *data,
      float  ray_origin[3],
      float  ray_direction[3])
{
  RigEntity *entity, *selected_entity = NULL;
  RigComponent *mesh;
  uint8_t *vertex_data;
  int n_vertices;
  size_t stride;
  int index;
  int i;
  float distance, min_distance = G_MAXFLOAT;
  bool hit;
  float transformed_ray_origin[3];
  float transformed_ray_direction[3];
  static const char *names[11] = { "plane", "cube0", "cube1", "cube2",
                                  "cube3", "cube4", "cube5", "cube6",
                                  "cube7", "cube8", "cube9"}, *name;
  GList *l;

  for (l = data->pickables, i = 0; l; l = l->next, i++)
    {
      entity = l->data;

      /* transform the ray into the model space */
      memcpy (transformed_ray_origin, ray_origin, 3 * sizeof (float));
      memcpy (transformed_ray_direction, ray_direction, 3 * sizeof (float));
      transform_ray (rig_entity_get_transform (entity),
                     TRUE, /* inverse of the transform */
                     transformed_ray_origin,
                     transformed_ray_direction);

      /* intersect the transformed ray with the mesh data */
      mesh = rig_entity_get_component (entity,
                                       RIG_COMPONENT_TYPE_MESH_RENDERER);
      vertex_data =
        rig_mesh_renderer_get_vertex_data (RIG_MESH_RENDERER (mesh), &stride);
      n_vertices = rig_mesh_renderer_get_n_vertices (RIG_MESH_RENDERER (mesh));

      hit = rig_util_intersect_mesh (vertex_data,
                                     n_vertices,
                                     stride,
                                     transformed_ray_origin,
                                     transformed_ray_direction,
                                     &index,
                                     &distance);

      /* to compare intersection distances we need to re-transform it back
       * to the world space, */
      cogl_vector3_normalize (transformed_ray_direction);
      transformed_ray_direction[0] *= distance;
      transformed_ray_direction[1] *= distance;
      transformed_ray_direction[2] *= distance;

      rig_util_transform_normal (rig_entity_get_transform (entity),
                                 &transformed_ray_direction[0],
                                 &transformed_ray_direction[1],
                                 &transformed_ray_direction[2]);
      distance = cogl_vector3_magnitude (transformed_ray_direction);

      if (hit && distance < min_distance)
        {
          min_distance = distance;
          selected_entity = entity;
          name = names[i];
        }
    }

  if (selected_entity)
    {
      g_message ("Hit the %s, triangle #%d, distance %.2f",
                 name, index, distance);
    }

  return selected_entity;
}

static void
update_camera_position (Data *data)
{
  /* Calculate where the origin currently is from the camera's
   * point of view. Then we can fixup the camera's position
   * so this matches the real position of the origin. */
  float relative_origin[3] = { 0, 0, -data->main_camera_z };
  rig_entity_get_transformed_position (data->main_camera,
                                       relative_origin);

  rig_entity_translate (data->main_camera,
                        -relative_origin[0],
                        -relative_origin[1],
                        -relative_origin[2]);
}

static RigInputEventStatus
test_input_handler (RigInputEvent *event, void *user_data)
{
  Data *data = user_data;
  RigMotionEventAction action;
  RigButtonState state;
  RigInputEventStatus status = RIG_INPUT_EVENT_STATUS_UNHANDLED;
  float x, y;

  switch (rig_input_event_get_type (event))
    {
      case RIG_INPUT_EVENT_TYPE_MOTION:
      {
        action = rig_motion_event_get_action (event);
        state = rig_motion_event_get_button_state (event);
        x = rig_motion_event_get_x (event);
        y = rig_motion_event_get_y (event);

        if (action == RIG_MOTION_EVENT_ACTION_DOWN &&
            state == RIG_BUTTON_STATE_2)
          {
            data->saved_rotation = *rig_entity_get_rotation (data->main_camera);

            cogl_quaternion_init_identity (&data->arcball.q_drag);

            rig_arcball_mouse_down (&data->arcball, data->fb_width - x, y);

            data->button_down = TRUE;

            status = RIG_INPUT_EVENT_STATUS_HANDLED;
          }
        else if (action == RIG_MOTION_EVENT_ACTION_DOWN &&
                 state == RIG_BUTTON_STATE_1)
          {
            /* pick */
            RigComponent *camera;
            float ray_position[3], ray_direction[3], screen_pos[2],
                  z_far, z_near;
            const float *viewport;
            CoglMatrix *camera_transform;
            const CoglMatrix *inverse_projection;

            camera = rig_entity_get_component (data->main_camera,
                                               RIG_COMPONENT_TYPE_CAMERA);
            viewport = rig_camera_get_viewport (RIG_CAMERA (camera));
            z_near = rig_camera_get_near_plane (RIG_CAMERA (camera));
            z_far = rig_camera_get_far_plane (RIG_CAMERA (camera));
            inverse_projection =
              rig_camera_get_inverse_projection (RIG_CAMERA (camera));

            camera_transform = rig_entity_get_transform (data->main_camera);

            screen_pos[0] = x;
            screen_pos[1] = y;

            rig_util_create_pick_ray (viewport,
                                      inverse_projection,
                                      camera_transform,
                                      screen_pos,
                                      ray_position,
                                      ray_direction);

            if (data->picking_ray)
              cogl_object_unref (data->picking_ray);
            data->picking_ray = create_picking_ray (data,
                                                    data->fb,
                                                    ray_position,
                                                    ray_direction,
                                                    z_far - z_near);

            data->selected_entity = pick (data, ray_position, ray_direction);
            if (data->selected_entity == NULL)
              rig_tool_update (data->tool, NULL);

            //status = RIG_INPUT_EVENT_STATUS_HANDLED;
          }
        else if (action == RIG_MOTION_EVENT_ACTION_MOVE &&
                 state == RIG_BUTTON_STATE_2)
          {
            CoglQuaternion new_rotation;

            if (!data->button_down)
              break;

            rig_arcball_mouse_motion (&data->arcball, data->fb_width - x, y);

            cogl_quaternion_multiply (&new_rotation,
                                      &data->saved_rotation,
                                      &data->arcball.q_drag);

            rig_entity_set_rotation (data->main_camera, &new_rotation);

            /* XXX: The remaining problem is calculating the new
             * position for the camera!
             *
             * If we transform the point (0, 0, camera_z) by the
             * camera's transform we can find where the origin is
             * relative to the camera, and then find out how far that
             * point is from the true origin so we know how to
             * translate the camera.
             */
            update_camera_position (data);

            status = RIG_INPUT_EVENT_STATUS_HANDLED;
          }
        else if (action == RIG_MOTION_EVENT_ACTION_DOWN &&
                 state == RIG_BUTTON_STATE_WHEELUP)
          {
            data->main_camera_z += 5;
            update_camera_position (data);
          }
        else if (action == RIG_MOTION_EVENT_ACTION_DOWN &&
                 state == RIG_BUTTON_STATE_WHEELDOWN)
          {
            if (data->main_camera_z >= 5)
              data->main_camera_z -= 5;
            update_camera_position (data);
          }
        else if (action == RIG_MOTION_EVENT_ACTION_UP)
          {
            data->button_down = FALSE;
          }

      break;
      }

      case RIG_INPUT_EVENT_TYPE_KEY:
      {
        RigKeyEventAction action;
        int32_t key;

        key = rig_key_event_get_keysym (event);
        action = rig_key_event_get_action (event);

        switch (key)
          {
          case RIG_KEY_p:
            if (action == RIG_KEY_EVENT_ACTION_UP)
              {
                data->edit = !data->edit;
                status = RIG_INPUT_EVENT_STATUS_UNHANDLED;
              }
            break;
          case RIG_KEY_minus:
            data->main_camera_z += 5;
            update_camera_position (data);
            break;
          case RIG_KEY_equal:
            if (data->main_camera_z >= 5)
              data->main_camera_z -= 5;
            update_camera_position (data);
            break;

          default:
            break;
          }
      }
    break;

      default:
        break;
    }

  return status;
}

#ifdef __ANDROID__

void
android_main (struct android_app *application)
{
  Data data;

  /* Make sure glue isn't stripped */
  app_dummy ();

  g_android_init ();

  memset (&data, 0, sizeof (Data));

  data.shell = rig_android_shell_new (application,
                                      test_init,
                                      test_fini,
                                      test_paint,
                                      &data);

  rig_shell_set_input_callback (data.shell, test_input_handler, &data);

  rig_shell_main (data.shell);
}

#else

int
main (int argc, char **argv)
{
  Data data;

  memset (&data, 0, sizeof (Data));

  data.shell = rig_shell_new (test_init, test_fini, test_paint, &data);

  rig_shell_set_input_callback (data.shell, test_input_handler, &data);

  rig_shell_main (data.shell);

  return 0;
}

#endif
