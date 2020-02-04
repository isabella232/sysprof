/* sysprof-memory-page.h
 *
 * Copyright 2016-2019 Christian Hergert <chergert@redhat.com>
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

#include <gtk/gtk.h>
#include <sysprof.h>

#include "sysprof-page.h"

G_BEGIN_DECLS

#define SYSPROF_TYPE_MEMORY_PAGE (sysprof_memory_page_get_type())

G_DECLARE_DERIVABLE_TYPE (SysprofMemoryPage, sysprof_memory_page, SYSPROF, MEMORY_PAGE, SysprofPage)

struct _SysprofMemoryPageClass
{
  SysprofPageClass parent_class;

  void (*go_previous) (SysprofMemoryPage *self);

  /*< private >*/
  gpointer _reserved[16];
};

GtkWidget            *sysprof_memory_page_new             (void);
SysprofMemoryProfile *sysprof_memory_page_get_profile     (SysprofMemoryPage    *self);
void                  sysprof_memory_page_set_profile     (SysprofMemoryPage    *self,
                                                           SysprofMemoryProfile *profile);
gchar                *sysprof_memory_page_screenshot      (SysprofMemoryPage    *self);
guint                 sysprof_memory_page_get_n_functions (SysprofMemoryPage    *self);

G_END_DECLS
