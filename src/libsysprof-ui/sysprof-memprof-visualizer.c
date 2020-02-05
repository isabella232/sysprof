/* sysprof-memprof-visualizer.c
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

#include "config.h"

#define G_LOG_DOMAIN "sysprof-memprof-visualizer"

#include "sysprof-memprof-visualizer.h"

typedef struct
{
  guint64 time;
  guint64 size;
} Memstat;

struct _SysprofMemprofVisualizer
{
  SysprofVisualizer     parent_instance;

  SysprofCaptureReader *reader;

  gint64                begin_time;
  gint64                duration;
};

G_DEFINE_TYPE (SysprofMemprofVisualizer, sysprof_memprof_visualizer, SYSPROF_TYPE_VISUALIZER)

static void
load_data_cb (GObject      *object,
              GAsyncResult *result,
              gpointer      user_data)
{
  SysprofMemprofVisualizer *self = (SysprofMemprofVisualizer *)object;

  g_assert (SYSPROF_IS_MEMPROF_VISUALIZER (self));
  g_assert (G_IS_TASK (result));

#if 0
  if ((pc = g_task_propagate_pointer (G_TASK (result), NULL)))
    {
      g_clear_pointer (&self->cache, point_cache_unref);
      self->cache = g_steal_pointer (&pc);
    }
#endif

  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static gboolean
collect_allocs_cb (const SysprofCaptureFrame *frame,
                   gpointer                   user_data)
{
  GArray *ar = user_data;

  g_assert (frame != NULL);
  g_assert (ar != NULL);

  return TRUE;
}

static void
sysprof_memprof_visualizer_worker (GTask        *task,
                                   gpointer      source_object,
                                   gpointer      task_data,
                                   GCancellable *cancellable)
{
  SysprofMemprofVisualizer *self = source_object;
  SysprofCaptureCursor *cursor = task_data;
  GArray *ar;

  g_assert (G_IS_TASK (task));
  g_assert (SYSPROF_IS_MEMPROF_VISUALIZER (self));
  g_assert (cursor != NULL);
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));

  ar = g_array_new (FALSE, FALSE, sizeof (Memstat));
  sysprof_capture_cursor_foreach (cursor, collect_allocs_cb, ar);

  g_task_return_pointer (task,
                         g_steal_pointer (&ar),
                         (GDestroyNotify) g_array_unref);
}

static void
sysprof_memprof_visualizer_set_reader (SysprofVisualizer    *visualizer,
                                       SysprofCaptureReader *reader)
{
  SysprofMemprofVisualizer *self = (SysprofMemprofVisualizer *)visualizer;
  static const SysprofCaptureFrameType types[] = {
    SYSPROF_CAPTURE_FRAME_MEMORY_ALLOC,
    SYSPROF_CAPTURE_FRAME_MEMORY_FREE,
  };
  g_autoptr(SysprofCaptureCursor) cursor = NULL;
  g_autoptr(GTask) task = NULL;
  SysprofCaptureCondition *c;

  g_assert (SYSPROF_IS_MEMPROF_VISUALIZER (self));
  g_assert (reader != NULL);

  self->begin_time = sysprof_capture_reader_get_start_time (reader);
  self->duration = sysprof_capture_reader_get_end_time (reader)
                 - sysprof_capture_reader_get_start_time (reader);

  cursor = sysprof_capture_cursor_new (reader);
  c = sysprof_capture_condition_new_where_type_in (G_N_ELEMENTS (types), types);
  sysprof_capture_cursor_add_condition (cursor, g_steal_pointer (&c));

  task = g_task_new (self, NULL, load_data_cb, NULL);
  g_task_set_source_tag (task, sysprof_memprof_visualizer_set_reader);
  g_task_set_task_data (task,
                        g_steal_pointer (&cursor),
                        (GDestroyNotify)sysprof_capture_cursor_unref);
  g_task_run_in_thread (task, sysprof_memprof_visualizer_worker);
}

SysprofMemprofVisualizer *
sysprof_memprof_visualizer_new (void)
{
  return g_object_new (SYSPROF_TYPE_MEMPROF_VISUALIZER, NULL);
}

static void
sysprof_memprof_visualizer_finalize (GObject *object)
{
  SysprofMemprofVisualizer *self = (SysprofMemprofVisualizer *)object;

  G_OBJECT_CLASS (sysprof_memprof_visualizer_parent_class)->finalize (object);
}

static void
sysprof_memprof_visualizer_class_init (SysprofMemprofVisualizerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  SysprofVisualizerClass *visualizer_class = SYSPROF_VISUALIZER_CLASS (klass);

  object_class->finalize = sysprof_memprof_visualizer_finalize;

  visualizer_class->set_reader = sysprof_memprof_visualizer_set_reader;
}

static void
sysprof_memprof_visualizer_init (SysprofMemprofVisualizer *self)
{
}
