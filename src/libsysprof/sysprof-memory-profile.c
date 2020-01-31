/* sysprof-memory-profile.c
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

#define G_LOG_DOMAIN "sysprof-memory-profile"

#include "config.h"

#include <sysprof-capture.h>

#include "sysprof-memory-profile.h"

#include "rax.h"

struct _SysprofMemoryProfile
{
  GObject               parent_instance;
  SysprofCaptureReader *reader;
  rax *rax;
};

typedef struct
{
  SysprofCaptureReader *reader;
  rax *rax;
} Generate;

static void profile_iface_init (SysprofProfileInterface *iface);

G_DEFINE_TYPE_WITH_CODE (SysprofMemoryProfile, sysprof_memory_profile, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (SYSPROF_TYPE_PROFILE, profile_iface_init))

static void
generate_free (Generate *g)
{
  g_clear_pointer (&g->reader, sysprof_capture_reader_unref);
  g_clear_pointer (&g->rax, raxFree);
  g_slice_free (Generate, g);
}

static void
sysprof_memory_profile_finalize (GObject *object)
{
  SysprofMemoryProfile *self = (SysprofMemoryProfile *)object;

  g_clear_pointer (&self->reader, sysprof_capture_reader_unref);
  g_clear_pointer (&self->rax, raxFree);

  G_OBJECT_CLASS (sysprof_memory_profile_parent_class)->finalize (object);
}

static void
sysprof_memory_profile_class_init (SysprofMemoryProfileClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = sysprof_memory_profile_finalize;
}

static void
sysprof_memory_profile_init (SysprofMemoryProfile *self)
{
}

SysprofProfile *
sysprof_memory_profile_new (void)
{
  return g_object_new (SYSPROF_TYPE_MEMORY_PROFILE, NULL);
}

static void
sysprof_memory_profile_set_reader (SysprofProfile       *profile,
                                   SysprofCaptureReader *reader)
{
  SysprofMemoryProfile *self = (SysprofMemoryProfile *)profile;

  g_assert (SYSPROF_IS_MEMORY_PROFILE (self));
  g_assert (reader != NULL);

  if (reader != self->reader)
    {
      g_clear_pointer (&self->reader, sysprof_capture_reader_unref);
      self->reader = sysprof_capture_reader_ref (reader);
    }
}

static SysprofCaptureCursor *
create_cursor (SysprofCaptureReader *reader)
{
  static SysprofCaptureFrameType types[] = {
    SYSPROF_CAPTURE_FRAME_MEMORY_ALLOC,
    SYSPROF_CAPTURE_FRAME_MEMORY_FREE,
  };
  SysprofCaptureCursor *cursor;
  SysprofCaptureCondition *cond;

  cond = sysprof_capture_condition_new_where_type_in (G_N_ELEMENTS (types), types);
  cursor = sysprof_capture_cursor_new (reader);
  sysprof_capture_cursor_add_condition (cursor, cond);

  return cursor;
}

static gboolean
cursor_foreach_cb (const SysprofCaptureFrame *frame,
                   gpointer                   user_data)
{
  Generate *g = user_data;

  g_assert (frame != NULL);
  g_assert (frame->type == SYSPROF_CAPTURE_FRAME_MEMORY_ALLOC ||
            frame->type == SYSPROF_CAPTURE_FRAME_MEMORY_FREE);

  if (frame->type == SYSPROF_CAPTURE_FRAME_MEMORY_ALLOC)
    {
      const SysprofCaptureMemoryAlloc *ev = (const SysprofCaptureMemoryAlloc *)frame;

      raxInsert (g->rax,
                 (guint8 *)&ev->alloc_addr,
                 sizeof ev->alloc_addr,
                 (gpointer)ev->alloc_size,
                 NULL);
    }
  else if (frame->type == SYSPROF_CAPTURE_FRAME_MEMORY_FREE)
    {
      const SysprofCaptureMemoryFree *ev = (const SysprofCaptureMemoryFree *)frame;

      raxRemove (g->rax,
                 (guint8 *)&ev->alloc_addr,
                 sizeof ev->alloc_addr,
                 NULL);
    }

  return TRUE;
}

static void
sysprof_memory_profile_generate_worker (GTask        *task,
                                        gpointer      source_object,
                                        gpointer      task_data,
                                        GCancellable *cancellable)
{
  SysprofCaptureCursor *cursor;
  Generate *g = task_data;

  g_assert (G_IS_TASK (task));
  g_assert (g != NULL);
  g_assert (g->reader != NULL);
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));

  cursor = create_cursor (g->reader);
  sysprof_capture_cursor_foreach (cursor, cursor_foreach_cb, g);

  g_task_return_boolean (task, TRUE);
}

static void
sysprof_memory_profile_generate (SysprofProfile      *profile,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  SysprofMemoryProfile *self = (SysprofMemoryProfile *)profile;
  g_autoptr(GTask) task = NULL;
  Generate *g;

  g_assert (SYSPROF_IS_MEMORY_PROFILE (self));
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_assert (self->rax == NULL);

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, sysprof_memory_profile_generate);

  if (self->reader == NULL)
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_NOT_INITIALIZED,
                               "No capture reader has been set");
      return;
    }

  g = g_slice_new0 (Generate);
  g->reader = sysprof_capture_reader_copy (self->reader);
  g->rax = raxNew ();

  g_task_set_task_data (task, g, (GDestroyNotify) generate_free);
  g_task_run_in_thread (task, sysprof_memory_profile_generate_worker);
}

static gboolean
sysprof_memory_profile_generate_finish (SysprofProfile  *profile,
                                        GAsyncResult    *result,
                                        GError         **error)
{
  SysprofMemoryProfile *self = (SysprofMemoryProfile *)profile;
  Generate *g;

  g_assert (SYSPROF_IS_MEMORY_PROFILE (self));
  g_assert (G_IS_TASK (result));

  if ((g = g_task_get_task_data (G_TASK (result))))
    self->rax = g_steal_pointer (&g->rax);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
profile_iface_init (SysprofProfileInterface *iface)
{
  iface->set_reader = sysprof_memory_profile_set_reader;
  iface->generate = sysprof_memory_profile_generate;
  iface->generate_finish = sysprof_memory_profile_generate_finish;
}

gpointer
sysprof_memory_profile_get_native (SysprofMemoryProfile *self)
{
  return self->rax;
}
