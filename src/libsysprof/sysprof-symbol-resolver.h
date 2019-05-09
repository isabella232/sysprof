/* sysprof-symbol-resolver.h
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

#if !defined (SYSPROF_INSIDE) && !defined (SYSPROF_COMPILATION)
# error "Only <sysprof.h> can be included directly."
#endif

#include <glib-object.h>

#include "sysprof-address.h"
#include "sysprof-capture-reader.h"
#include "sysprof-version-macros.h"

G_BEGIN_DECLS

#define SYSPROF_TYPE_SYMBOL_RESOLVER (sysprof_symbol_resolver_get_type())

SYSPROF_AVAILABLE_IN_ALL
G_DECLARE_INTERFACE (SysprofSymbolResolver, sysprof_symbol_resolver, SYSPROF, SYMBOL_RESOLVER, GObject)

struct _SysprofSymbolResolverInterface
{
  GTypeInterface parent_interface;

  void   (*load)                 (SysprofSymbolResolver *self,
                                  SysprofCaptureReader  *reader);
  gchar *(*resolve)              (SysprofSymbolResolver *self,
                                  guint64                time,
                                  GPid                   pid,
                                  SysprofCaptureAddress  address,
                                  GQuark                *tag);
  gchar *(*resolve_with_context) (SysprofSymbolResolver *self,
                                  guint64                time,
                                  GPid                   pid,
                                  SysprofAddressContext  context,
                                  SysprofCaptureAddress  address,
                                  GQuark                *tag);
};

SYSPROF_AVAILABLE_IN_ALL
void   sysprof_symbol_resolver_load                 (SysprofSymbolResolver *self,
                                                     SysprofCaptureReader  *reader);
SYSPROF_AVAILABLE_IN_ALL
gchar *sysprof_symbol_resolver_resolve              (SysprofSymbolResolver *self,
                                                     guint64                time,
                                                     GPid                   pid,
                                                     SysprofCaptureAddress  address,
                                                     GQuark                *tag);
SYSPROF_AVAILABLE_IN_ALL
gchar *sysprof_symbol_resolver_resolve_with_context (SysprofSymbolResolver *self,
                                                     guint64                time,
                                                     GPid                   pid,
                                                     SysprofAddressContext  context,
                                                     SysprofCaptureAddress  address,
                                                     GQuark                *tag);

G_END_DECLS
