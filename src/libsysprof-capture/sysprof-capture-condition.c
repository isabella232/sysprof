/* sysprof-capture-condition.c
 *
 * Copyright 2016-2019 Christian Hergert <chergert@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * Subject to the terms and conditions of this license, each copyright holder
 * and contributor hereby grants to those receiving rights under this license
 * a perpetual, worldwide, non-exclusive, no-charge, royalty-free,
 * irrevocable (except for failure to satisfy the conditions of this license)
 * patent license to make, have made, use, offer to sell, sell, import, and
 * otherwise transfer this software, where such license applies only to those
 * patent claims, already acquired or hereafter acquired, licensable by such
 * copyright holder or contributor that are necessarily infringed by:
 *
 * (a) their Contribution(s) (the licensed copyrights of copyright holders
 *     and non-copyrightable additions of contributors, in source or binary
 *     form) alone; or
 *
 * (b) combination of their Contribution(s) with the work of authorship to
 *     which such Contribution(s) was added by such copyright holder or
 *     contributor, if, at the time the Contribution is added, such addition
 *     causes such combination to be necessarily infringed. The patent license
 *     shall not apply to any other combinations which include the
 *     Contribution.
 *
 * Except as expressly stated above, no rights or licenses from any copyright
 * holder or contributor is granted under this license, whether expressly, by
 * implication, estoppel or otherwise.
 *
 * DISCLAIMER
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#define G_LOG_DOMAIN "sysprof-capture-condition"

#include "config.h"

#include <string.h>

#include "sysprof-capture-condition.h"

/**
 * SECTION:sysprof-capture-condition
 * @title: SysprofCaptureCondition
 *
 * The #SysprofCaptureCondition type is an abstraction on an operation
 * for a sort of AST to the #SysprofCaptureCursor. The goal is that if
 * we abstract the types of fields we want to match in the cursor
 * that we can opportunistically add indexes to speed up the operation
 * later on without changing the implementation of cursor consumers.
 */

typedef enum
{
  SYSPROF_CAPTURE_CONDITION_AND,
  SYSPROF_CAPTURE_CONDITION_OR,
  SYSPROF_CAPTURE_CONDITION_WHERE_TYPE_IN,
  SYSPROF_CAPTURE_CONDITION_WHERE_TIME_BETWEEN,
  SYSPROF_CAPTURE_CONDITION_WHERE_PID_IN,
  SYSPROF_CAPTURE_CONDITION_WHERE_COUNTER_IN,
  SYSPROF_CAPTURE_CONDITION_WHERE_FILE,
} SysprofCaptureConditionType;

struct _SysprofCaptureCondition
{
  volatile gint ref_count;
  SysprofCaptureConditionType type;
  union {
    GArray *where_type_in;
    struct {
      gint64 begin;
      gint64 end;
    } where_time_between;
    GArray *where_pid_in;
    GArray *where_counter_in;
    struct {
      SysprofCaptureCondition *left;
      SysprofCaptureCondition *right;
    } and, or;
    gchar *where_file;
  } u;
};

gboolean
sysprof_capture_condition_match (const SysprofCaptureCondition *self,
                                 const SysprofCaptureFrame     *frame)
{
  g_assert (self != NULL);
  g_assert (frame != NULL);

  switch (self->type)
    {
    case SYSPROF_CAPTURE_CONDITION_AND:
      return sysprof_capture_condition_match (self->u.and.left, frame) &&
             sysprof_capture_condition_match (self->u.and.right, frame);

    case SYSPROF_CAPTURE_CONDITION_OR:
      return sysprof_capture_condition_match (self->u.or.left, frame) ||
             sysprof_capture_condition_match (self->u.or.right, frame);

    case SYSPROF_CAPTURE_CONDITION_WHERE_TYPE_IN:
      for (guint i = 0; i < self->u.where_type_in->len; i++)
        {
          if (frame->type == g_array_index (self->u.where_type_in, SysprofCaptureFrameType, i))
            return TRUE;
        }
      return FALSE;

    case SYSPROF_CAPTURE_CONDITION_WHERE_TIME_BETWEEN:
      return (frame->time >= self->u.where_time_between.begin && frame->time <= self->u.where_time_between.end);

    case SYSPROF_CAPTURE_CONDITION_WHERE_PID_IN:
      for (guint i = 0; i < self->u.where_pid_in->len; i++)
        {
          if (frame->pid == g_array_index (self->u.where_pid_in, gint32, i))
            return TRUE;
        }
      return FALSE;

    case SYSPROF_CAPTURE_CONDITION_WHERE_COUNTER_IN:
      if (frame->type == SYSPROF_CAPTURE_FRAME_CTRSET)
        {
          const SysprofCaptureCounterSet *set = (SysprofCaptureCounterSet *)frame;

          for (guint i = 0; i < self->u.where_counter_in->len; i++)
            {
              guint counter = g_array_index (self->u.where_counter_in, guint, i);

              for (guint j = 0; j < set->n_values; j++)
                {
                  if (counter == set->values[j].ids[0] ||
                      counter == set->values[j].ids[1] ||
                      counter == set->values[j].ids[2] ||
                      counter == set->values[j].ids[3] ||
                      counter == set->values[j].ids[4] ||
                      counter == set->values[j].ids[5] ||
                      counter == set->values[j].ids[6] ||
                      counter == set->values[j].ids[7])
                    return TRUE;
                }
            }
        }
      else if (frame->type == SYSPROF_CAPTURE_FRAME_CTRDEF)
        {
          const SysprofCaptureCounterDefine *def = (SysprofCaptureCounterDefine *)frame;

          for (guint i = 0; i < self->u.where_counter_in->len; i++)
            {
              guint counter = g_array_index (self->u.where_counter_in, guint, i);

              for (guint j = 0; j < def->n_counters; j++)
                {
                  if (def->counters[j].id == counter)
                    return TRUE;
                }
            }
        }

      return FALSE;

    case SYSPROF_CAPTURE_CONDITION_WHERE_FILE:
      if (frame->type != SYSPROF_CAPTURE_FRAME_FILE_CHUNK)
        return FALSE;

      return g_strcmp0 (((const SysprofCaptureFileChunk *)frame)->path, self->u.where_file) == 0;

    default:
      break;
    }

  g_assert_not_reached ();

  return FALSE;
}

static SysprofCaptureCondition *
sysprof_capture_condition_init (void)
{
  SysprofCaptureCondition *self;

  self = g_slice_new0 (SysprofCaptureCondition);
  self->ref_count = 1;

  return g_steal_pointer (&self);
}

SysprofCaptureCondition *
sysprof_capture_condition_copy (const SysprofCaptureCondition *self)
{
  switch (self->type)
    {
    case SYSPROF_CAPTURE_CONDITION_AND:
      return sysprof_capture_condition_new_and (
        sysprof_capture_condition_copy (self->u.and.left),
        sysprof_capture_condition_copy (self->u.and.right));

    case SYSPROF_CAPTURE_CONDITION_OR:
      return sysprof_capture_condition_new_or (
        sysprof_capture_condition_copy (self->u.or.left),
        sysprof_capture_condition_copy (self->u.or.right));

    case SYSPROF_CAPTURE_CONDITION_WHERE_TYPE_IN:
      return sysprof_capture_condition_new_where_type_in (
          self->u.where_type_in->len,
          (const SysprofCaptureFrameType *)(gpointer)self->u.where_type_in->data);

    case SYSPROF_CAPTURE_CONDITION_WHERE_TIME_BETWEEN:
      return sysprof_capture_condition_new_where_time_between (
        self->u.where_time_between.begin,
        self->u.where_time_between.end);

    case SYSPROF_CAPTURE_CONDITION_WHERE_PID_IN:
      return sysprof_capture_condition_new_where_pid_in (
          self->u.where_pid_in->len,
          (const gint32 *)(gpointer)self->u.where_pid_in->data);

    case SYSPROF_CAPTURE_CONDITION_WHERE_COUNTER_IN:
      return sysprof_capture_condition_new_where_counter_in (
          self->u.where_counter_in->len,
          (const guint *)(gpointer)self->u.where_counter_in->data);

    case SYSPROF_CAPTURE_CONDITION_WHERE_FILE:
      return sysprof_capture_condition_new_where_file (self->u.where_file);

    default:
      break;
    }

  g_return_val_if_reached (NULL);
}

static void
sysprof_capture_condition_finalize (SysprofCaptureCondition *self)
{
  switch (self->type)
    {
    case SYSPROF_CAPTURE_CONDITION_AND:
      sysprof_capture_condition_unref (self->u.and.left);
      sysprof_capture_condition_unref (self->u.and.right);
      break;

    case SYSPROF_CAPTURE_CONDITION_OR:
      sysprof_capture_condition_unref (self->u.or.left);
      sysprof_capture_condition_unref (self->u.or.right);
      break;

    case SYSPROF_CAPTURE_CONDITION_WHERE_TYPE_IN:
      g_array_free (self->u.where_type_in, TRUE);
      break;

    case SYSPROF_CAPTURE_CONDITION_WHERE_TIME_BETWEEN:
      break;

    case SYSPROF_CAPTURE_CONDITION_WHERE_PID_IN:
      g_array_free (self->u.where_pid_in, TRUE);
      break;

    case SYSPROF_CAPTURE_CONDITION_WHERE_COUNTER_IN:
      g_array_free (self->u.where_counter_in, TRUE);
      break;

    case SYSPROF_CAPTURE_CONDITION_WHERE_FILE:
      g_free (self->u.where_file);
      break;

    default:
      g_assert_not_reached ();
      break;
    }

  g_slice_free (SysprofCaptureCondition, self);
}

SysprofCaptureCondition *
sysprof_capture_condition_ref (SysprofCaptureCondition *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  g_return_val_if_fail (self->ref_count > 0, NULL);

  g_atomic_int_inc (&self->ref_count);
  return self;
}

void
sysprof_capture_condition_unref (SysprofCaptureCondition *self)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (self->ref_count > 0);

  if (g_atomic_int_dec_and_test (&self->ref_count))
    sysprof_capture_condition_finalize (self);
}

SysprofCaptureCondition *
sysprof_capture_condition_new_where_type_in (guint                     n_types,
                                        const SysprofCaptureFrameType *types)
{
  SysprofCaptureCondition *self;

  g_return_val_if_fail (types != NULL, NULL);

  self = sysprof_capture_condition_init ();
  self->type = SYSPROF_CAPTURE_CONDITION_WHERE_TYPE_IN;
  self->u.where_type_in = g_array_sized_new (FALSE, FALSE, sizeof (SysprofCaptureFrameType), n_types);
  g_array_set_size (self->u.where_type_in, n_types);
  memcpy (self->u.where_type_in->data, types, sizeof (SysprofCaptureFrameType) * n_types);

  return self;
}

SysprofCaptureCondition *
sysprof_capture_condition_new_where_time_between (gint64 begin_time,
                                             gint64 end_time)
{
  SysprofCaptureCondition *self;

  if G_UNLIKELY (begin_time > end_time)
    {
      gint64 tmp = begin_time;
      begin_time = end_time;
      end_time = tmp;
    }

  self = sysprof_capture_condition_init ();
  self->type = SYSPROF_CAPTURE_CONDITION_WHERE_TIME_BETWEEN;
  self->u.where_time_between.begin = begin_time;
  self->u.where_time_between.end = end_time;

  return self;
}

SysprofCaptureCondition *
sysprof_capture_condition_new_where_pid_in (guint         n_pids,
                                       const gint32 *pids)
{
  SysprofCaptureCondition *self;

  g_return_val_if_fail (pids != NULL, NULL);

  self = sysprof_capture_condition_init ();
  self->type = SYSPROF_CAPTURE_CONDITION_WHERE_PID_IN;
  self->u.where_pid_in = g_array_sized_new (FALSE, FALSE, sizeof (gint32), n_pids);
  g_array_set_size (self->u.where_pid_in, n_pids);
  memcpy (self->u.where_pid_in->data, pids, sizeof (gint32) * n_pids);

  return self;
}

SysprofCaptureCondition *
sysprof_capture_condition_new_where_counter_in (guint        n_counters,
                                           const guint *counters)
{
  SysprofCaptureCondition *self;

  g_return_val_if_fail (counters != NULL || n_counters == 0, NULL);

  self = sysprof_capture_condition_init ();
  self->type = SYSPROF_CAPTURE_CONDITION_WHERE_COUNTER_IN;
  self->u.where_counter_in = g_array_sized_new (FALSE, FALSE, sizeof (guint), n_counters);

  if (n_counters > 0)
    {
      g_array_set_size (self->u.where_counter_in, n_counters);
      memcpy (self->u.where_counter_in->data, counters, sizeof (guint) * n_counters);
    }

  return self;
}

/**
 * sysprof_capture_condition_new_and:
 * @left: (transfer full): An #SysprofCaptureCondition
 * @right: (transfer full): An #SysprofCaptureCondition
 *
 * Creates a new #SysprofCaptureCondition that requires both left and right
 * to evaluate to %TRUE.
 *
 * Returns: (transfer full): A new #SysprofCaptureCondition.
 */
SysprofCaptureCondition *
sysprof_capture_condition_new_and (SysprofCaptureCondition *left,
                                   SysprofCaptureCondition *right)
{
  SysprofCaptureCondition *self;

  g_return_val_if_fail (left != NULL, NULL);
  g_return_val_if_fail (right != NULL, NULL);

  self = sysprof_capture_condition_init ();
  self->type = SYSPROF_CAPTURE_CONDITION_AND;
  self->u.and.left = left;
  self->u.and.right = right;

  return self;
}

/**
 * sysprof_capture_condition_new_or:
 * @left: (transfer full): An #SysprofCaptureCondition
 * @right: (transfer full): An #SysprofCaptureCondition
 *
 * Creates a new #SysprofCaptureCondition that requires either left and right
 * to evaluate to %TRUE.
 *
 * Returns: (transfer full): A new #SysprofCaptureCondition.
 */
SysprofCaptureCondition *
sysprof_capture_condition_new_or (SysprofCaptureCondition *left,
                                  SysprofCaptureCondition *right)
{
  SysprofCaptureCondition *self;

  g_return_val_if_fail (left != NULL, NULL);
  g_return_val_if_fail (right != NULL, NULL);

  self = sysprof_capture_condition_init ();
  self->type = SYSPROF_CAPTURE_CONDITION_OR;
  self->u.or.left = left;
  self->u.or.right = right;

  return self;
}

/**
 * sysprof_capture_condition_new_where_file:
 * @path: a file path to lookup
 *
 * Creates a new condition that matches #SysprofCaptureFileChunk frames
 * which contain the path @path.
 *
 * Returns: (transfer full): a new #SysprofCaptureCondition
 */
SysprofCaptureCondition *
sysprof_capture_condition_new_where_file (const gchar *path)
{
  SysprofCaptureCondition *self;

  g_return_val_if_fail (path != NULL, NULL);

  self = sysprof_capture_condition_init ();
  self->type = SYSPROF_CAPTURE_CONDITION_WHERE_FILE;
  self->u.where_file = g_strdup (path);

  return self;
}
