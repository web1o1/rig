/*
 * Rut
 *
 * Copyright (C) 2013  Intel Corporation
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

#ifndef _RIG_SIMULATOR_H_
#define _RIG_SIMULATOR_H_

#include <rut.h>
typedef struct _RigSimulator RigSimulator;

#include "rig-engine.h"

/*
 * Simulator actions are sent back as requests to the frontend at the
 * end of a frame.
 */
typedef enum _RigSimulatorActionType
{
  RIG_SIMULATOR_ACTION_TYPE_SET_PLAY_MODE=1,
  RIG_SIMULATOR_ACTION_TYPE_SELECT_OBJECT,
} RigSimulatorActionType;

/* The "simulator" is the process responsible for updating object
 * properties either in response to user input, the progression of
 * animations or running other forms of simulation such as physics.
 */
struct _RigSimulator
{
  RigFrontendID frontend_id;

  /* when running as an editor or slave device then the UI
   * can be edited at runtime and we handle some things a
   * bit differently. For example we only need to be able
   * to map ids to objects to support editing operations.
   */
  bool editable;

  RutShell *shell;
  RutContext *ctx;
  RigEngine *engine;

  int fd;
  RigRPCPeer *simulator_peer;

  float view_x;
  float view_y;

  float last_pointer_x;
  float last_pointer_y;

  RutButtonState button_state;

  RigPBUnSerializer *unserializer;

  GHashTable *id_map;

  /* Only initialized and maintained if editable == true */
  GHashTable *object_map;

  RutList actions;
  int n_actions;
};

void
rig_simulator_init (RutShell *shell, void *user_data);

void
rig_simulator_fini (RutShell *shell, void *user_data);

void
rig_simulator_run_frame (RutShell *shell, void *user_data);

void
rig_simulator_stop_service (RigSimulator *simulator);

void
rig_simulator_stop_service (RigSimulator *simulator);

void
rig_simulator_action_set_play_mode_enabled (RigSimulator *simulator,
                                            bool enabled);

void
rig_simulator_action_select_object (RigSimulator *simulator,
                                    RutObject *object,
                                    RutSelectAction action);

#endif /* _RIG_SIMULATOR_H_ */