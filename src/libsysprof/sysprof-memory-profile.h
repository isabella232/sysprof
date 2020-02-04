/* sysprof-memory-profile.h
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#if !defined (SYSPROF_INSIDE) && !defined (SYSPROF_COMPILATION)
# error "Only <sysprof.h> can be included directly."
#endif

#include "sysprof-version-macros.h"

#include "sysprof-profile.h"
#include "sysprof-selection.h"

G_BEGIN_DECLS

#define SYSPROF_TYPE_MEMORY_PROFILE (sysprof_memory_profile_get_type())

SYSPROF_AVAILABLE_IN_ALL
G_DECLARE_FINAL_TYPE (SysprofMemoryProfile, sysprof_memory_profile, SYSPROF, MEMORY_PROFILE, GObject)

SYSPROF_AVAILABLE_IN_3_36
SysprofProfile *sysprof_memory_profile_new                (void);
SYSPROF_AVAILABLE_IN_3_36
SysprofProfile *sysprof_memory_profile_new_with_selection (SysprofSelection *selection);
SYSPROF_AVAILABLE_IN_3_36
gpointer        sysprof_memory_profile_get_native         (SysprofMemoryProfile *self);
SYSPROF_AVAILABLE_IN_3_36
gpointer        sysprof_memory_profile_get_stash          (SysprofMemoryProfile *self);
SYSPROF_AVAILABLE_IN_3_36
gboolean        sysprof_memory_profile_is_empty           (SysprofMemoryProfile *self);
SYSPROF_AVAILABLE_IN_3_36
GQuark          sysprof_memory_profile_get_tag            (SysprofMemoryProfile *self,
                                                           const gchar          *symbol);

G_END_DECLS
