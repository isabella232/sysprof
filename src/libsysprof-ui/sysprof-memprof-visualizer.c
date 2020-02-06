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
  cairo_surface_t      *surface;
  SysprofCaptureReader *reader;
  GtkAllocation         alloc;
  gint64                begin_time;
  gint64                duration;
  GdkRGBA               fg;
  GdkRGBA               bg;
} DrawContext;

struct _SysprofMemprofVisualizer
{
  SysprofVisualizer     parent_instance;

  SysprofCaptureReader *reader;
  GCancellable         *cancellable;

  cairo_surface_t      *surface;
  gint                  surface_w;
  gint                  surface_h;

  guint                 queued_draw;

  gint64                begin_time;
  gint64                duration;
};

G_DEFINE_TYPE (SysprofMemprofVisualizer, sysprof_memprof_visualizer, SYSPROF_TYPE_VISUALIZER)

static void
draw_context_finalize (DrawContext *draw)
{
  g_clear_pointer (&draw->surface, cairo_surface_destroy);
}

static void
draw_context_unref (DrawContext *draw)
{
  g_atomic_rc_box_release_full (draw, (GDestroyNotify)draw_context_finalize);
}

static DrawContext *
draw_context_ref (DrawContext *draw)
{
  return g_atomic_rc_box_acquire (draw);
}

static void
sysprof_memprof_visualizer_set_reader (SysprofVisualizer    *visualizer,
                                       SysprofCaptureReader *reader)
{
  SysprofMemprofVisualizer *self = (SysprofMemprofVisualizer *)visualizer;

  g_assert (SYSPROF_IS_MEMPROF_VISUALIZER (self));

  if (reader == self->reader)
    return;

  g_clear_pointer (&self->reader, sysprof_capture_reader_unref);

  self->reader = sysprof_capture_reader_ref (reader);
  self->begin_time = sysprof_capture_reader_get_start_time (reader);
  self->duration = sysprof_capture_reader_get_end_time (reader)
                 - sysprof_capture_reader_get_start_time (reader);

  gtk_widget_queue_draw (GTK_WIDGET (self));
}

SysprofMemprofVisualizer *
sysprof_memprof_visualizer_new (void)
{
  return g_object_new (SYSPROF_TYPE_MEMPROF_VISUALIZER, NULL);
}

static void
draw_context_add (cairo_t                        *cr,
                  DrawContext                    *draw,
                  const SysprofCaptureAllocation *ev)
{
  gint x;
  gint y;

  g_assert (draw != NULL);
  g_assert (ev != NULL);

  x = (ev->frame.time - draw->begin_time) / (gdouble)draw->duration * draw->alloc.width;
  y = 0;

  cairo_rectangle (cr, x, y, 1, 1);
}

static void
sysprof_memprof_visualizer_draw_worker (GTask        *task,
                                        gpointer      source_object,
                                        gpointer      task_data,
                                        GCancellable *cancellable)
{
  DrawContext *draw = task_data;
  SysprofCaptureFrameType type;
  cairo_t *cr;
  guint counter = 0;

  g_assert (G_IS_TASK (task));
  g_assert (draw != NULL);
  g_assert (draw->surface != NULL);
  g_assert (draw->reader != NULL);
  g_assert (!cancellable || G_IS_CANCELLABLE (cancellable));

  cr = cairo_create (draw->surface);

  /* Fill background first */
  gdk_cairo_rectangle (cr, &draw->alloc);
  gdk_cairo_set_source_rgba (cr, &draw->bg);
  cairo_fill (cr);

  /* Now draw data points */
  while (sysprof_capture_reader_peek_type (draw->reader, &type))
    {
      const SysprofCaptureAllocation *ev;

      /* Cancellation check every 1000 frames */
      if G_UNLIKELY (++counter == 1000)
        {
          if (g_task_return_error_if_cancelled (task))
            {
              cairo_destroy (cr);
              return;
            }

          counter = 0;
        }

      /* We only care about memory frames here */
      if (type != SYSPROF_CAPTURE_FRAME_MEMORY_ALLOC &&
          type != SYSPROF_CAPTURE_FRAME_MEMORY_FREE)
        {
          if (sysprof_capture_reader_skip (draw->reader))
            continue;
          break;
        }

      if (type == SYSPROF_CAPTURE_FRAME_MEMORY_ALLOC)
        ev = sysprof_capture_reader_read_memory_alloc (draw->reader);
      else
        ev = sysprof_capture_reader_read_memory_free (draw->reader);

      if (ev == NULL)
        break;

      draw_context_add (cr, draw, ev);
    }

  /* Now fill our draw paths */
  gdk_cairo_set_source_rgba (cr, &draw->fg);
  cairo_fill (cr);
  cairo_destroy (cr);

  g_task_return_boolean (task, TRUE);
}

static void
draw_finished (GObject      *object,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr(SysprofMemprofVisualizer) self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (object == NULL);
  g_assert (G_IS_TASK (result));
  g_assert (SYSPROF_IS_MEMPROF_VISUALIZER (self));

  if (g_task_propagate_boolean (G_TASK (result), &error))
    {
      DrawContext *draw = g_task_get_task_data (G_TASK (result));

      g_clear_pointer (&self->surface, cairo_surface_destroy);

      self->surface = g_steal_pointer (&draw->surface);
      self->surface_w = draw->alloc.width;
      self->surface_h = draw->alloc.height;

      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static gboolean
sysprof_memprof_visualizer_begin_draw (SysprofMemprofVisualizer *self)
{
  g_autoptr(GTask) task = NULL;
  GtkStyleContext *style_context;
  GtkAllocation alloc;
  GtkStateFlags state;
  DrawContext *draw;
  GdkWindow *window;
  gint scale;

  g_assert (SYSPROF_IS_MEMPROF_VISUALIZER (self));

  self->queued_draw = 0;

  /* Make sure we even need to draw */
  gtk_widget_get_allocation (GTK_WIDGET (self), &alloc);
  if (self->reader == NULL ||
      !gtk_widget_get_visible (GTK_WIDGET (self)) ||
      !gtk_widget_get_mapped (GTK_WIDGET (self)) ||
      alloc.width == 0 || alloc.height == 0)
    return G_SOURCE_REMOVE;

  draw = g_atomic_rc_box_new0 (DrawContext);
  draw->alloc.width = alloc.width;
  draw->alloc.height = alloc.height;
  draw->reader = sysprof_capture_reader_copy (self->reader);
  draw->begin_time = self->begin_time;
  draw->duration = self->duration;

  /* Create a surface to draw to */
  window = gtk_widget_get_window (GTK_WIDGET (self));
  scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
  draw->surface = gdk_window_create_similar_image_surface (window, CAIRO_FORMAT_RGB24, alloc.width, alloc.height, scale);

  /* Get our styles to render with so we can do off-main-thread */
  state = gtk_widget_get_state_flags (GTK_WIDGET (self));
  style_context = gtk_widget_get_style_context (GTK_WIDGET (self));
  gtk_style_context_get_color (style_context, state, &draw->fg);
  /* Takin the render cost for render_background() isn't worth it on the
   * main thread. The main thing we want here is an opaque surface that we
   * can quickly draw as a texture when rendering.
   */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gtk_style_context_get_background_color (style_context, state, &draw->bg);
  G_GNUC_END_IGNORE_DEPRECATIONS

  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  task = g_task_new (NULL, self->cancellable, draw_finished, g_object_ref (self));
  g_task_set_source_tag (task, sysprof_memprof_visualizer_begin_draw);
  g_task_set_task_data (task, g_steal_pointer (&draw), (GDestroyNotify)draw_context_unref);
  g_task_run_in_thread (task, sysprof_memprof_visualizer_draw_worker);

  return G_SOURCE_REMOVE;
}

static void
sysprof_memprof_visualizer_queue_redraw (SysprofMemprofVisualizer *self)
{
  g_assert (SYSPROF_IS_MEMPROF_VISUALIZER (self));

  if (self->queued_draw == 0)
    self->queued_draw = g_idle_add_full (G_PRIORITY_HIGH_IDLE,
                                         (GSourceFunc) sysprof_memprof_visualizer_begin_draw,
                                         g_object_ref (self),
                                         g_object_unref);
}

static void
sysprof_memprof_visualizer_size_allocate (GtkWidget     *widget,
                                          GtkAllocation *alloc)
{
  SysprofMemprofVisualizer *self = (SysprofMemprofVisualizer *)widget;

  g_assert (GTK_IS_WIDGET (widget));
  g_assert (alloc != NULL);

  GTK_WIDGET_CLASS (sysprof_memprof_visualizer_parent_class)->size_allocate (widget, alloc);

  sysprof_memprof_visualizer_queue_redraw (self);
}

static void
sysprof_memprof_visualizer_destroy (GtkWidget *widget)
{
  SysprofMemprofVisualizer *self = (SysprofMemprofVisualizer *)widget;

  g_clear_pointer (&self->reader, sysprof_capture_reader_unref);
  g_clear_pointer (&self->surface, cairo_surface_destroy);
  g_clear_handle_id (&self->queued_draw, g_source_remove);

  GTK_WIDGET_CLASS (sysprof_memprof_visualizer_parent_class)->destroy (widget);
}

static gboolean
sysprof_memprof_visualizer_draw (GtkWidget *widget,
                                 cairo_t   *cr)
{
  SysprofMemprofVisualizer *self = (SysprofMemprofVisualizer *)widget;

  g_assert (SYSPROF_IS_MEMPROF_VISUALIZER (self));
  g_assert (cr != NULL);

  if (self->surface != NULL)
    {
      GtkAllocation alloc;

      gtk_widget_get_allocation (widget, &alloc);

      cairo_save (cr);
      cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
      cairo_rectangle (cr, 0, 0, alloc.width, alloc.height);

      /* We might be drawing an updated image in the background, and this
       * will take our current surface (which is the wrong size) and draw
       * it stretched to fit the allocation. That gives us *something* that
       * represents the end result even if it is a bit blurry in the mean
       * time. Allocators take a while to render anyway.
       */
      if (self->surface_w != alloc.width || self->surface_h != alloc.height)
        {
          cairo_scale (cr,
                       (gdouble)alloc.width / (gdouble)self->surface_w,
                       (gdouble)alloc.height / (gdouble)self->surface_h);
        }

      cairo_set_source_surface (cr, self->surface, 0, 0);
      cairo_fill (cr);
      cairo_restore (cr);

      return GDK_EVENT_PROPAGATE;
    }

  return GTK_WIDGET_CLASS (sysprof_memprof_visualizer_parent_class)->draw (widget, cr);
}

static void
sysprof_memprof_visualizer_class_init (SysprofMemprofVisualizerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  SysprofVisualizerClass *visualizer_class = SYSPROF_VISUALIZER_CLASS (klass);

  widget_class->destroy = sysprof_memprof_visualizer_destroy;
  widget_class->draw = sysprof_memprof_visualizer_draw;
  widget_class->size_allocate = sysprof_memprof_visualizer_size_allocate;

  visualizer_class->set_reader = sysprof_memprof_visualizer_set_reader;
}

static void
sysprof_memprof_visualizer_init (SysprofMemprofVisualizer *self)
{
}
