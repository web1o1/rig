/*
 * Rig
 *
 * Copyright (C) 2013 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
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
 */

#include <config.h>

#include <rut.h>

typedef struct _RigBindingView RigBindingView;

typedef struct _Dependency
{
  RigBindingView *binding_view;

  RutObject *object;
  RutProperty *property;

  bool preview;

  RutBoxLayout *hbox;
  RutText *label;
  RutText *variable_name_label;

} Dependency;

struct _RigBindingView
{
  RutObjectProps _parent;

  RutContext *ctx;

  int ref_count;

  RutGraphableProps graphable;

  RutStack *top_stack;
  RutDragBin *drag_bin;

  RutBoxLayout *vbox;

  RutBoxLayout *dependencies_vbox;

  RutStack *drop_stack;
  RutInputRegion *drop_region;
  RutText *drop_label;

  RutText *code_view;

  RutProperty *preview_dependency_prop;
  GList *dependencies;

};

static void
_rig_binding_view_free (void *object)
{
  RigBindingView *binding_view = object;
  //RigControllerView *view = binding_view->view;

  rut_graphable_destroy (binding_view);

  g_slice_free (RigBindingView, binding_view);
}

RutType rig_binding_view_type;

static void
_rig_binding_view_init_type (void)
{
  static RutGraphableVTable graphable_vtable = { 0 };

  static RutSizableVTable sizable_vtable = {
    rut_composite_sizable_set_size,
    rut_composite_sizable_get_size,
    rut_composite_sizable_get_preferred_width,
    rut_composite_sizable_get_preferred_height,
    rut_composite_sizable_add_preferred_size_callback
  };

  RutType *type = &rig_binding_view_type;
#define TYPE RigBindingView

  rut_type_init (type, G_STRINGIFY (TYPE));
  rut_type_add_refable (type, ref_count, _rig_binding_view_free);
  rut_type_add_interface (type,
                          RUT_INTERFACE_ID_GRAPHABLE,
                          offsetof (TYPE, graphable),
                          &graphable_vtable);
  rut_type_add_interface (type,
                          RUT_INTERFACE_ID_SIZABLE,
                          0, /* no implied properties */
                          &sizable_vtable);
  rut_type_add_interface (type,
                          RUT_INTERFACE_ID_COMPOSITE_SIZABLE,
                          offsetof (TYPE, top_stack),
                          NULL); /* no vtable */

#undef TYPE
}

static void
remove_dependency (RigBindingView *binding_view,
                   RutProperty *property)
{
  GList *l;

  for (l = binding_view->dependencies; l; l = l->next)
    {
      Dependency *dependency = l->data;
      if (dependency->property == property)
        {
          rut_box_layout_remove (binding_view->dependencies_vbox, dependency->hbox);
          rut_refable_unref (dependency->object);
          g_slice_free (Dependency, dependency);
          return;
        }
    }

  g_warn_if_reached ();
}

static void
on_dependency_delete_button_click_cb (RutIconButton *button, void *user_data)
{
  Dependency *dependency = user_data;

  remove_dependency (dependency->binding_view,
                     dependency->property);
}

static Dependency *
add_dependency (RigBindingView *binding_view,
                RutProperty *property,
                bool drag_preview)
{
  Dependency *dependency = g_slice_new0 (Dependency);
  RutProperty *label_prop;
  char *dependency_label;
  RutObject *object = property->object;
  RutBin *bin;
  const char *component_str = NULL;
  const char *label_str;

  dependency->object = rut_refable_ref (object);
  dependency->binding_view = binding_view;

  dependency->property = property;

  dependency->preview = drag_preview;

  dependency->hbox = rut_box_layout_new (binding_view->ctx,
                                         RUT_BOX_LAYOUT_PACKING_LEFT_TO_RIGHT);

  if (!drag_preview)
    {
      RutIconButton *delete_button =
        rut_icon_button_new (binding_view->ctx,
                             NULL, /* label */
                             0, /* ignore label position */
                             "delete-white.png", /* normal */
                             "delete-white.png", /* hover */
                             "delete.png", /* active */
                             "delete-white.png"); /* disabled */
      rut_box_layout_add (dependency->hbox, false, delete_button);
      rut_refable_unref (delete_button);
      rut_icon_button_add_on_click_callback (delete_button,
                                             on_dependency_delete_button_click_cb,
                                             dependency,
                                             NULL); /* destroy notify */
    }

  /* XXX:
   * We want a clear way of showing a relationship to an object +
   * property here.
   *
   * Just showing a property name isn't really enough
   * */

  if (rut_object_is (object, RUT_INTERFACE_ID_COMPONENTABLE))
    {
      RutComponentableProps *component =
        rut_object_get_properties (object, RUT_INTERFACE_ID_COMPONENTABLE);
      RutEntity *entity = component->entity;
      label_prop = rut_introspectable_lookup_property (entity, "label");
      /* XXX: Hack to drop the "Rut" prefix from the name... */
      component_str = rut_object_get_type_name (object) + 3;
    }
  else
    label_prop = rut_introspectable_lookup_property (object, "label");

  if (label_prop)
    label_str = rut_property_get_text (label_prop);
  else
    label_str = "<Object>";

  if (component_str)
    {
      dependency_label = g_strdup_printf ("%s::%s::%s",
                                          label_str,
                                          component_str,
                                          property->spec->name);
    }
  else
    {
      dependency_label = g_strdup_printf ("%s::%s",
                                          label_str,
                                          property->spec->name);
    }

  dependency->label = rut_text_new_with_text (binding_view->ctx, NULL,
                                              dependency_label);
  g_free (dependency_label);
  rut_box_layout_add (dependency->hbox, false, dependency->label);
  rut_refable_unref (dependency->label);

  bin = rut_bin_new (binding_view->ctx);
  rut_bin_set_left_padding (bin, 20);
  rut_box_layout_add (dependency->hbox, false, bin);
  rut_refable_unref (bin);

  /* TODO: Check if the name is unique for the current binding... */
  dependency->variable_name_label =
    rut_text_new_with_text (binding_view->ctx, NULL,
                            property->spec->name);
  rut_text_set_editable (dependency->variable_name_label, true);
  rut_bin_set_child (bin, dependency->variable_name_label);
  rut_refable_unref (dependency->variable_name_label);

  binding_view->dependencies =
    g_list_prepend (binding_view->dependencies, dependency);

  rut_box_layout_add (binding_view->dependencies_vbox,
                      false, dependency->hbox);
  rut_refable_unref (dependency->hbox);

  return dependency;
}

static RutInputEventStatus
drop_region_input_cb (RutInputRegion *region,
                      RutInputEvent *event,
                      void *user_data)
{
  RigBindingView *binding_view = user_data;

  if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_DROP_OFFER)
    {
      RutObject *payload = rut_drop_offer_event_get_payload (event);

      if (rut_object_get_type (payload) == &rut_prop_inspector_type)
        {
          RutProperty *property = rut_prop_inspector_get_property (payload);

          g_print ("Drop Offer\n");

          binding_view->preview_dependency_prop = property;
          add_dependency (binding_view, property, true);

          rut_shell_take_drop_offer (binding_view->ctx->shell,
                                     binding_view->drop_region);
          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
    }
  else if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_DROP)
    {
      RutObject *payload = rut_drop_offer_event_get_payload (event);

      /* We should be able to assume a _DROP_OFFER was accepted before
       * we'll be sent a _DROP */
      g_warn_if_fail (binding_view->preview_dependency_prop);

      remove_dependency (binding_view, binding_view->preview_dependency_prop);
      binding_view->preview_dependency_prop = NULL;

      if (rut_object_get_type (payload) == &rut_prop_inspector_type)
        {
          RutProperty *property = rut_prop_inspector_get_property (payload);

          add_dependency (binding_view, property, false);

          return RUT_INPUT_EVENT_STATUS_HANDLED;
        }
    }
  else if (rut_input_event_get_type (event) == RUT_INPUT_EVENT_TYPE_DROP_CANCEL)
    {
      /* NB: This may be cleared by a _DROP */
      if (binding_view->preview_dependency_prop)
        {
          remove_dependency (binding_view, binding_view->preview_dependency_prop);
          binding_view->preview_dependency_prop = NULL;
        }

      return RUT_INPUT_EVENT_STATUS_HANDLED;
    }

  return RUT_INPUT_EVENT_STATUS_UNHANDLED;
}

RigBindingView *
rig_binding_view_new (RutContext *ctx)
{
  RigBindingView *binding_view =
    rut_object_alloc0 (RigBindingView,
                       &rig_binding_view_type,
                       _rig_binding_view_init_type);
  RutBin *dependencies_indent;
  RutBoxLayout *hbox;
  RutText *equals;

  binding_view->ref_count = 1;
  binding_view->ctx = ctx;

  rut_graphable_init (binding_view);

  binding_view->top_stack = rut_stack_new (ctx, 1, 1);
  rut_graphable_add_child (binding_view, binding_view->top_stack);
  rut_refable_unref (binding_view->top_stack);

  binding_view->vbox =
    rut_box_layout_new (ctx, RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
  rut_stack_add (binding_view->top_stack, binding_view->vbox);
  rut_refable_unref (binding_view->vbox);

  binding_view->drop_stack = rut_stack_new (ctx, 1, 1);
  rut_box_layout_add (binding_view->vbox, false, binding_view->drop_stack);
  rut_refable_unref (binding_view->drop_stack);

  binding_view->drop_label = rut_text_new_with_text (ctx, NULL, "Dependencies...");
  rut_stack_add (binding_view->drop_stack, binding_view->drop_label);
  rut_refable_unref (binding_view->drop_label);

  binding_view->drop_region =
    rut_input_region_new_rectangle (0, 0, 1, 1,
                                    drop_region_input_cb,
                                    binding_view);
  rut_stack_add (binding_view->drop_stack, binding_view->drop_region);
  rut_refable_unref (binding_view->drop_region);

  dependencies_indent = rut_bin_new (ctx);
  rut_box_layout_add (binding_view->vbox, false, dependencies_indent);
  rut_refable_unref (dependencies_indent);
  rut_bin_set_left_padding (dependencies_indent, 10);

  binding_view->dependencies_vbox =
    rut_box_layout_new (ctx, RUT_BOX_LAYOUT_PACKING_TOP_TO_BOTTOM);
  rut_bin_set_child (dependencies_indent, binding_view->dependencies_vbox);
  rut_refable_unref (binding_view->dependencies_vbox);

  hbox = rut_box_layout_new (ctx, RUT_BOX_LAYOUT_PACKING_LEFT_TO_RIGHT);
  rut_box_layout_add (binding_view->vbox, false, hbox);
  rut_refable_unref (hbox);

  equals = rut_text_new_with_text (ctx, "bold", "=");
  rut_box_layout_add (hbox, false, equals);
  rut_refable_unref (equals);

  binding_view->code_view = rut_text_new_with_text (ctx, "monospace", "");
  rut_text_set_hint_text (binding_view->code_view, "Expression...");
  rut_text_set_editable (binding_view->code_view, true);
  rut_box_layout_add (hbox, false, binding_view->code_view);
  rut_refable_unref (binding_view->code_view);

  return binding_view;
}
