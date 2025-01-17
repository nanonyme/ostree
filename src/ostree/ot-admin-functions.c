/*
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
 *
 * SPDX-License-Identifier: LGPL-2.0+
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
 * License along with this library. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "libglnx.h"
#include "ot-admin-functions.h"
#include "otutil.h"
#include "ostree.h"

gboolean
ot_admin_require_booted_deployment_or_osname (OstreeSysroot       *sysroot,
                                              const char          *osname,
                                              GCancellable        *cancellable,
                                              GError             **error)
{
  OstreeDeployment *booted_deployment =
    ostree_sysroot_get_booted_deployment (sysroot);
  if (booted_deployment == NULL && osname == NULL)
      return glnx_throw (error, "Not currently booted into an OSTree system and no --os= argument given");
  return TRUE;
}

/**
 * ot_admin_checksum_version:
 * @checksum: A GVariant from an ostree checksum.
 *
 *
 * Get the version metadata string from a commit variant object, if it exists.
 *
 * Returns: A newly allocated string of the version, or %NULL is none
 */
char *
ot_admin_checksum_version (GVariant *checksum)
{
  g_autoptr(GVariant) metadata = NULL;
  const char *ret = NULL;

  metadata = g_variant_get_child_value (checksum, 0);

  if (!g_variant_lookup (metadata, OSTREE_COMMIT_META_KEY_VERSION, "&s", &ret))
    return NULL;

  return g_strdup (ret);
}

OstreeDeployment *
ot_admin_get_indexed_deployment (OstreeSysroot  *sysroot,
                                 int             index,
                                 GError        **error)

{
  g_autoptr(GPtrArray) current_deployments =
    ostree_sysroot_get_deployments (sysroot);

  if (index < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Invalid index %d", index);
      return NULL;
    }
  if (index >= current_deployments->len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Out of range deployment index %d, expected < %d", index,
                   current_deployments->len);
      return NULL;
    }

  return g_object_ref (current_deployments->pdata[index]);
}

struct ContextState {
  GMainContext *mainctx;
  gboolean running;
};

static gboolean
on_sysroot_lock_timeout (gpointer user_data)
{
  g_print ("Waiting for sysroot lock...\n");
  return TRUE;
}

static void
on_sysroot_lock_acquired (OstreeSysroot       *sysroot,
                          GAsyncResult        *result,
                          struct ContextState *state)
{
  state->running = FALSE;
  g_main_context_wakeup (state->mainctx);
}

gboolean
ot_admin_sysroot_lock (OstreeSysroot  *sysroot,
                       GError        **error)
{
  gboolean ret = FALSE;
  gboolean acquired;
  struct ContextState state = {
    .mainctx = g_main_context_new (),
    .running = TRUE,
  };

  g_main_context_push_thread_default (state.mainctx);

  if (!ostree_sysroot_try_lock (sysroot, &acquired, error))
    goto out;

  if (!acquired)
    {
      GSource *timeout_src = g_timeout_source_new_seconds (3);
      g_source_set_callback (timeout_src, (GSourceFunc)on_sysroot_lock_timeout, &state, NULL);
      g_source_attach (timeout_src, state.mainctx);
      g_source_unref (timeout_src);

      on_sysroot_lock_timeout (&state);

      ostree_sysroot_lock_async (sysroot, NULL, (GAsyncReadyCallback)on_sysroot_lock_acquired, &state);

      while (state.running)
        g_main_context_iteration (state.mainctx, TRUE);
    }

  ret = TRUE;
 out:
  g_main_context_pop_thread_default (state.mainctx);
  g_main_context_unref (state.mainctx);
  return ret;
}

gboolean
ot_admin_execve_reboot (OstreeSysroot *sysroot, GError **error)
{
  OstreeDeployment *booted = ostree_sysroot_get_booted_deployment (sysroot);

  /* If the sysroot isn't booted, we shouldn't reboot, even if somehow the user
   * asked for it; might accidentally be specified in a build script, etc.
   */
  if (!booted)
    return TRUE;

  if (execlp ("systemctl", "systemctl", "reboot", NULL) < 0)
    return glnx_throw_errno_prefix (error, "execve(systemctl reboot)");
  return TRUE;
}
