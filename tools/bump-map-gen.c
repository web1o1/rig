/*
 * Bump Map Generator Tool
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
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *   Plamena Manolova <plamena.n.manolova@intel.com>
 *   Robert Bragg <robert@linux.intel.com>
 */

/*
 * This simple tool can be used to either generate a bump/height map
 * from a colour image or to generate a normal map from a bump/height
 * map.
 *
 * Usage:
 * bump-map-gen [OPTION...] INPUT_FILE
 *
 * Help Options:
 *   -h, --help                  Show help options
 *
 * Application Options:
 *   -b, --generate-bump-map     Create Bump Map
 *   -o, --output                Output
 *
 * Examples:
 *
 * To create a bump/height map you can do:
 *   ./bump-map-gen -b -o my-bump-map.png my-src-image.png
 *
 * To create a normal map from a bump/height map do:
 *   ./bump-map-gen -o my-normal-map.png my-src-bump-map.png
 *
 */

#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include <cogl/cogl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

static gboolean bump_map_mode = FALSE;
static char *output = NULL;
static char **remaining_args = NULL;

static const GOptionEntry options[] =
{
  { "generate-bump-map", 'b', 0, 0,
    &bump_map_mode, "Create Bump Map" },
  { "output", 'o', 0, G_OPTION_ARG_STRING,
    &output, "Output" },
  { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY,
    &remaining_args, "Input File", "INPUT_FILE" },
  { 0 }
};

const char *
get_extension (const char *path)
{
  const char *ext = strrchr (path, '.');
  return ext ? ext + 1 : NULL;
}

const char *
choose_output_file_type (const char *path)
{
  const char *ext = get_extension (path);
  if (!ext)
    return "png";
  else if (strcmp (ext, "jpg") == 0)
    return "jpeg";
  else
    return ext;
}

static void
generate_bump_map (CoglContext *ctx,
                   const char *path,
                   const char *output)
{
  CoglBitmap *bitmap = cogl_bitmap_new_from_file (ctx, path, NULL);
  CoglTexture *src = cogl_texture_2d_new_from_bitmap (bitmap);

  int tex_width = cogl_texture_get_width (src);
  int tex_height = cogl_texture_get_height (src);

  CoglTexture *dst = cogl_texture_2d_new_with_size (ctx, tex_width, tex_height);

  CoglOffscreen *offscreen = cogl_offscreen_new_with_texture (dst);
  CoglFramebuffer *fb = offscreen;

  CoglPipeline *pipeline;
  CoglSnippet* snippet;

  uint8_t* pixels;
  int rowstride;
  GdkPixbuf* pixbuf;

  cogl_framebuffer_orthographic (fb, 0, 0, tex_width, tex_height, -1, 100);

  pipeline = cogl_pipeline_new (ctx);
  cogl_pipeline_set_layer_texture (pipeline, 0, src);

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
    /* definitions */
    NULL,

    /* post */
    "float grey = (cogl_color_out.r * 0.299 + cogl_color_out.g * 0.587 + cogl_color_out.b * 0.114);\n"
    "cogl_color_out = vec4 (grey, grey, grey, 1.0);\n");

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  cogl_framebuffer_clear4f (fb,
                            COGL_BUFFER_BIT_COLOR|COGL_BUFFER_BIT_DEPTH,
                            0, 0, 0, 0);
  cogl_framebuffer_draw_textured_rectangle (fb, pipeline,
                                            0, 0, tex_width, tex_height,
                                            0, 0, 1, 1);

  cogl_object_unref (pipeline);
  cogl_object_unref (offscreen);

  rowstride = tex_width * 3;
  pixels = (uint8_t *) malloc (rowstride * tex_height);
  cogl_texture_get_data (dst, COGL_PIXEL_FORMAT_RGB_888, 0, pixels);

  pixbuf = gdk_pixbuf_new_from_data (pixels, GDK_COLORSPACE_RGB, FALSE, 8,
                                     tex_width, tex_height, rowstride, NULL,
                                     NULL);
  gdk_pixbuf_save (pixbuf, output, choose_output_file_type (path), NULL, NULL);

  g_object_unref (pixbuf);
}

void
generate_normal_map (CoglContext *ctx,
                     const char *path,
                     const char *output)
{
  CoglBitmap *bitmap = cogl_bitmap_new_from_file (ctx, path, NULL);
  CoglTexture *src =
    cogl_texture_2d_new_from_bitmap (bitmap);

  int tex_width = cogl_texture_get_width (src);
  int tex_height = cogl_texture_get_height (src);

  CoglTexture *dst = cogl_texture_2d_new_with_size (ctx, tex_width, tex_height);

  CoglOffscreen *offscreen = cogl_offscreen_new_with_texture (dst);
  CoglFramebuffer *fb = offscreen;

  float pixel_width = 1.0 / tex_width;
  float pixel_height = 1.0 / tex_height;

  float sobel_kernel_x[9] = {-1,  0,  1,
                             -2,  0,  2,
                             -1,  0,  1};

  float sobel_kernel_y[9] = {-1, -2, -1,
                              0,  0,  0,
                              1,  2,  1};

  float offset_x[9] = { -pixel_width,  0.0, pixel_width,
                        -pixel_width,  0.0, pixel_width,
                        -pixel_width,  0.0, pixel_width};

  float offset_y[9] = {-pixel_height, -pixel_height, -pixel_height,
                        0.0,           0.0,           0.0,
                        pixel_height,  pixel_height,  pixel_height};

  CoglPipeline *pipeline;
  CoglSnippet* snippet;
  int location;

  uint8_t *pixels;
  int rowstride;
  GdkPixbuf *pixbuf;

  cogl_framebuffer_orthographic (fb, 0, 0, tex_width, tex_height, -1, 100);

  pipeline = cogl_pipeline_new (ctx);
  cogl_pipeline_set_layer_texture (pipeline, 0, src);

  /* We don't want this layer to automatically be sampled so we opt
   * out of Cogl's automatic layer combining by defining the layer
   * combine as a NOP...  */
  cogl_pipeline_set_layer_combine (pipeline, 0,
                                   "RGBA=REPLACE(PREVIOUS)", NULL);

  snippet = cogl_snippet_new (COGL_SNIPPET_HOOK_FRAGMENT,
    /* definitions */
    "#define KERNEL_SIZE 9\n"
    "uniform float sobel_kernel_x[KERNEL_SIZE];\n"
    "uniform float sobel_kernel_y[KERNEL_SIZE];\n"
    "uniform float offset_x[KERNEL_SIZE];\n"
    "uniform float offset_y[KERNEL_SIZE];\n",

    /* post */
    "vec2 UV = cogl_tex_coord0_in.st;\n"
    "vec4 final_color = vec4 (0.0);\n"
    "int i = 0;\n"

    "  for (i = 0; i < KERNEL_SIZE; i++)\n"
    "    {\n"
    "      vec4 frag_col = texture2D(cogl_sampler0, vec2(UV.x + offset_x[i], UV.y + offset_y[i]));\n"
    "      frag_col += texture2D(cogl_sampler0, vec2(UV.x + 2.0 * offset_x[i], UV.y + 2.0 * offset_y[i]));\n"
    "      frag_col += texture2D(cogl_sampler0, vec2(UV.x + 3.0 * offset_x[i], UV.y + 3.0 * offset_y[i]));\n"
    "      frag_col += texture2D(cogl_sampler0, vec2(UV.x + 4.0 * offset_x[i], UV.y + 4.0 * offset_y[i]));\n"
    "      frag_col += texture2D(cogl_sampler0, vec2(UV.x + 5.0 * offset_x[i], UV.y + 5.0 * offset_y[i]));\n"
    "      final_color  +=  vec4(frag_col.r * sobel_kernel_x[i], frag_col.r * sobel_kernel_y[i], 0.0, 0.0);\n"
    "    }\n"

    "final_color.z = 1.0;\n"
    "final_color.w = 1.0;\n"
    "final_color = (final_color + 1.0) / 2.0;\n"
    "cogl_color_out = final_color;\n");

  cogl_pipeline_add_snippet (pipeline, snippet);
  cogl_object_unref (snippet);

  location = cogl_pipeline_get_uniform_location (pipeline, "blurSize_x");
  cogl_pipeline_set_uniform_float (pipeline, location, 1, 1, &pixel_width);

  location = cogl_pipeline_get_uniform_location (pipeline, "blurSize_x");
  cogl_pipeline_set_uniform_float (pipeline, location, 1, 1, &pixel_height);

  location = cogl_pipeline_get_uniform_location (pipeline, "sobel_kernel_x");
  cogl_pipeline_set_uniform_float (pipeline, location, 1, 9, sobel_kernel_x);

  location = cogl_pipeline_get_uniform_location (pipeline, "sobel_kernel_y");
  cogl_pipeline_set_uniform_float (pipeline, location, 1, 9, sobel_kernel_y);

  location = cogl_pipeline_get_uniform_location (pipeline, "offset_x");
  cogl_pipeline_set_uniform_float (pipeline, location, 1, 9, offset_x);

  location = cogl_pipeline_get_uniform_location (pipeline, "offset_y");
  cogl_pipeline_set_uniform_float (pipeline, location, 1, 9, offset_y);

  cogl_framebuffer_clear4f (fb,
                            COGL_BUFFER_BIT_COLOR|COGL_BUFFER_BIT_DEPTH,
                            0, 0, 0, 0);

  cogl_framebuffer_draw_textured_rectangle (fb, pipeline,
                                            0, 0, tex_width, tex_height,
                                            0, 0, 1, 1);

  cogl_object_unref (pipeline);
  cogl_object_unref (offscreen);

  rowstride = tex_width * 3;
  pixels = (uint8_t *)malloc (rowstride * tex_height);
  cogl_texture_get_data (dst, COGL_PIXEL_FORMAT_RGB_888, 0, pixels);

  pixbuf = gdk_pixbuf_new_from_data (pixels, GDK_COLORSPACE_RGB, FALSE, 8,
                                     tex_width, tex_height, rowstride, NULL,
                                     NULL);
  gdk_pixbuf_save (pixbuf, output, choose_output_file_type (path), NULL, NULL);

  g_object_unref (pixbuf);
}

int
main (int argc, char **argv)
{
  CoglContext *ctx = cogl_context_new (NULL, NULL);
  GOptionContext *context = g_option_context_new (NULL);
  GError *error = NULL;

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("option parsing failed: %s\n", error->message);
      exit (EXIT_FAILURE);
    }

  if (remaining_args == NULL || remaining_args[1] != NULL)
    {
      g_printerr ("A single input file must be specified\n");
      exit (EXIT_FAILURE);
    }

  if (!output)
    {
      g_printerr ("An output file must be specified\n");
      exit (EXIT_FAILURE);
    }

  if (bump_map_mode)
    generate_bump_map (ctx, remaining_args[0], output);
  else
    generate_normal_map (ctx, remaining_args[0], output);

  return 0;
}
