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

#include "sysprof-capture-symbol-resolver.h"
#include "sysprof-elf-symbol-resolver.h"
#include "sysprof-kernel-symbol-resolver.h"
#include "sysprof-memory-profile.h"
#include "sysprof-symbol-resolver.h"

#include "rax.h"
#include "../stackstash.h"

typedef struct
{
  SysprofSelection     *selection;
  SysprofCaptureReader *reader;
  GPtrArray            *resolvers;
  GStringChunk         *symbols;
  GHashTable           *tags;
  StackStash           *stash;
  rax                  *rax;
  GArray               *resolved;
} Generate;

struct _SysprofMemoryProfile
{
  GObject               parent_instance;
  SysprofSelection     *selection;
  SysprofCaptureReader *reader;
  Generate             *g;
};

static void profile_iface_init (SysprofProfileInterface *iface);

G_DEFINE_TYPE_WITH_CODE (SysprofMemoryProfile, sysprof_memory_profile, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (SYSPROF_TYPE_PROFILE, profile_iface_init))

enum {
  PROP_0,
  PROP_SELECTION,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
generate_free (Generate *g)
{
  g_clear_pointer (&g->reader, sysprof_capture_reader_unref);
  g_clear_pointer (&g->rax, raxFree);
  g_clear_pointer (&g->stash, stack_stash_unref);
  g_clear_pointer (&g->resolvers, g_ptr_array_unref);
  g_clear_pointer (&g->symbols, g_string_chunk_free);
  g_clear_pointer (&g->tags, g_hash_table_unref);
  g_clear_pointer (&g->resolved, g_array_unref);
  g_clear_object (&g->selection);
  g_slice_free (Generate, g);
}

static Generate *
generate_ref (Generate *g)
{
  return g_atomic_rc_box_acquire (g);
}

static void
generate_unref (Generate *g)
{
  g_atomic_rc_box_release_full (g, (GDestroyNotify)generate_free);
}

static void
sysprof_memory_profile_finalize (GObject *object)
{
  SysprofMemoryProfile *self = (SysprofMemoryProfile *)object;

  g_clear_pointer (&self->g, generate_unref);
  g_clear_pointer (&self->reader, sysprof_capture_reader_unref);
  g_clear_object (&self->selection);

  G_OBJECT_CLASS (sysprof_memory_profile_parent_class)->finalize (object);
}

static void
sysprof_memory_profile_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  SysprofMemoryProfile *self = SYSPROF_MEMORY_PROFILE (object);

  switch (prop_id)
    {
    case PROP_SELECTION:
      g_value_set_object (value, self->selection);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
sysprof_memory_profile_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
  SysprofMemoryProfile *self = SYSPROF_MEMORY_PROFILE (object);

  switch (prop_id)
    {
    case PROP_SELECTION:
      self->selection = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
sysprof_memory_profile_class_init (SysprofMemoryProfileClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = sysprof_memory_profile_finalize;
  object_class->get_property = sysprof_memory_profile_get_property;
  object_class->set_property = sysprof_memory_profile_set_property;

  properties [PROP_SELECTION] =
    g_param_spec_object ("selection",
                         "Selection",
                         "The selection for filtering the callgraph",
                         SYSPROF_TYPE_SELECTION,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
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

  /* Short-circuit if we don't care about this frame */
  if (!sysprof_selection_contains (g->selection, frame->time))
    return TRUE;

  /* Handle removal frames */
  if (frame->type == SYSPROF_CAPTURE_FRAME_MEMORY_FREE)
    {
      const SysprofCaptureMemoryFree *ev = (const SysprofCaptureMemoryFree *)frame;

      raxRemove (g->rax,
                 (guint8 *)&ev->alloc_addr,
                 sizeof ev->alloc_addr,
                 NULL);

      return TRUE;
    }

  /* Handle memory allocations */
  if (frame->type == SYSPROF_CAPTURE_FRAME_MEMORY_ALLOC)
    {
      const SysprofCaptureMemoryAlloc *ev = (const SysprofCaptureMemoryAlloc *)frame;
      SysprofAddressContext last_context = SYSPROF_ADDRESS_CONTEXT_NONE;
      StackNode *node;
      guint len = 5;

      raxInsert (g->rax,
                 (guint8 *)&ev->alloc_addr,
                 sizeof ev->alloc_addr,
                 (gpointer)ev->alloc_size,
                 NULL);

      node = stack_stash_add_trace (g->stash, ev->addrs, ev->n_addrs, ev->alloc_size);

      for (const StackNode *iter = node; iter != NULL; iter = iter->parent)
        len++;

      if (G_UNLIKELY (g->resolved->len < len))
        g_array_set_size (g->resolved, len);

      len = 0;

      for (const StackNode *iter = node; iter != NULL; iter = iter->parent)
        {
          SysprofAddressContext context = SYSPROF_ADDRESS_CONTEXT_NONE;
          SysprofAddress address = iter->data;
          const gchar *symbol = NULL;

          if (sysprof_address_is_context_switch (address, &context))
            {
              if (last_context)
                symbol = sysprof_address_context_to_string (last_context);
              else
                symbol = NULL;

              last_context = context;
            }
          else
            {
              for (guint i = 0; i < g->resolvers->len; i++)
                {
                  SysprofSymbolResolver *resolver = g_ptr_array_index (g->resolvers, i);
                  GQuark tag = 0;
                  gchar *str;

                  str = sysprof_symbol_resolver_resolve_with_context (resolver,
                                                                      frame->time,
                                                                      frame->pid,
                                                                      last_context,
                                                                      address,
                                                                      &tag);

                  if (str != NULL)
                    {
                      symbol = g_string_chunk_insert_const (g->symbols, str);
                      g_free (str);

                      if (tag != 0 && !g_hash_table_contains (g->tags, symbol))
                        g_hash_table_insert (g->tags, (gchar *)symbol, GSIZE_TO_POINTER (tag));

                      break;
                    }
                }
            }

          if (symbol != NULL)
            g_array_index (g->resolved, SysprofAddress, len++) = POINTER_TO_U64 (symbol);
        }
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

  /* Make sure the capture is at the beginning */
  sysprof_capture_reader_reset (g->reader);

  /* Load all our symbol resolvers */
  for (guint i = 0; i < g->resolvers->len; i++)
    {
      SysprofSymbolResolver *resolver = g_ptr_array_index (g->resolvers, i);

      sysprof_symbol_resolver_load (resolver, g->reader);
      sysprof_capture_reader_reset (g->reader);
    }

  cursor = create_cursor (g->reader);
  sysprof_capture_cursor_foreach (cursor, cursor_foreach_cb, g);

  /* Release some data we don't need anymore */
  g_clear_pointer (&g->resolved, g_array_unref);
  g_clear_pointer (&g->resolvers, g_ptr_array_unref);
  g_clear_pointer (&g->reader, sysprof_capture_reader_unref);
  g_clear_object (&g->selection);

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

  g = g_atomic_rc_box_new0 (Generate);
  g->reader = sysprof_capture_reader_copy (self->reader);
  g->selection = sysprof_selection_copy (self->selection);
  g->rax = raxNew ();
  g->stash = stack_stash_new (NULL);
  g->resolvers = g_ptr_array_new_with_free_func (g_object_unref);
  g->symbols = g_string_chunk_new (4096*4);
  g->tags = g_hash_table_new (g_str_hash, g_str_equal);
  g->resolved = g_array_new (FALSE, TRUE, sizeof (guint64));

  g_ptr_array_add (g->resolvers, sysprof_capture_symbol_resolver_new ());
  g_ptr_array_add (g->resolvers, sysprof_kernel_symbol_resolver_new ());
  g_ptr_array_add (g->resolvers, sysprof_elf_symbol_resolver_new ());

  g_task_set_task_data (task, g, (GDestroyNotify) generate_unref);
  g_task_run_in_thread (task, sysprof_memory_profile_generate_worker);
}

static gboolean
sysprof_memory_profile_generate_finish (SysprofProfile  *profile,
                                        GAsyncResult    *result,
                                        GError         **error)
{
  SysprofMemoryProfile *self = (SysprofMemoryProfile *)profile;

  g_assert (SYSPROF_IS_MEMORY_PROFILE (self));
  g_assert (G_IS_TASK (result));

  g_clear_pointer (&self->g, generate_unref);

  if (g_task_propagate_boolean (G_TASK (result), error))
    {
      Generate *g = g_task_get_task_data (G_TASK (result));
      self->g = generate_ref (g);
      return TRUE;
    }

  return FALSE;
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
  g_return_val_if_fail (SYSPROF_IS_MEMORY_PROFILE (self), NULL);

  if (self->g != NULL)
    return self->g->rax;

  return NULL;
}

gpointer
sysprof_memory_profile_get_stash (SysprofMemoryProfile *self)
{
  g_return_val_if_fail (SYSPROF_IS_MEMORY_PROFILE (self), NULL);

  if (self->g != NULL)
    return self->g->stash;

  return NULL;
}

gboolean
sysprof_memory_profile_is_empty (SysprofMemoryProfile *self)
{
  StackNode *root;

  g_return_val_if_fail (SYSPROF_IS_MEMORY_PROFILE (self), FALSE);

  return (self->g == NULL ||
          self->g->stash == NULL ||
          !(root = stack_stash_get_root (self->g->stash)) ||
          !root->total);
}

GQuark
sysprof_memory_profile_get_tag (SysprofMemoryProfile *self,
                                const gchar          *symbol)
{
  g_return_val_if_fail (SYSPROF_IS_MEMORY_PROFILE (self), 0);

  if (self->g != NULL)
    return GPOINTER_TO_SIZE (g_hash_table_lookup (self->g->tags, symbol));

  return 0;
}

SysprofProfile *
sysprof_memory_profile_new_with_selection (SysprofSelection *selection)
{
  return g_object_new (SYSPROF_TYPE_MEMORY_PROFILE,
                       "selection", selection,
                       NULL);
}
