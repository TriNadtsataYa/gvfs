/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2011 Red Hat, Inc.
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gvfsudisks2drive.h"
#include "gvfsudisks2volume.h"
#include "gvfsudisks2mount.h"

typedef struct _GVfsUDisks2VolumeClass GVfsUDisks2VolumeClass;

struct _GVfsUDisks2VolumeClass
{
  GObjectClass parent_class;
};

struct _GVfsUDisks2Volume
{
  GObject parent;

  GVfsUDisks2VolumeMonitor *monitor; /* owned by volume monitor */
  GVfsUDisks2Mount         *mount;   /* owned by volume monitor */
  GVfsUDisks2Drive         *drive;   /* owned by volume monitor */

  UDisksBlock *block;

  /* set in update_volume() */
  GIcon *icon;
  GFile *activation_root;
  gchar *name;
  gchar *device_file;
  dev_t dev;
  gchar *uuid;
  gboolean can_mount;
  gboolean should_automount;
};

static void gvfs_udisks2_volume_volume_iface_init (GVolumeIface *iface);

static void on_block_changed (GObject    *object,
                              GParamSpec *pspec,
                              gpointer    user_data);

G_DEFINE_TYPE_EXTENDED (GVfsUDisks2Volume, gvfs_udisks2_volume, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME, gvfs_udisks2_volume_volume_iface_init))

static void
gvfs_udisks2_volume_finalize (GObject *object)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (object);

  if (volume->mount != NULL)
    {
      gvfs_udisks2_mount_unset_volume (volume->mount, volume);
    }

  if (volume->drive != NULL)
    {
      gvfs_udisks2_drive_unset_volume (volume->drive, volume);
    }

  g_signal_handlers_disconnect_by_func (volume->block, G_CALLBACK (on_block_changed), volume);
  g_object_unref (volume->block);

  if (volume->icon != NULL)
    g_object_unref (volume->icon);
  if (volume->activation_root != NULL)
    g_object_unref (volume->activation_root);

  g_free (volume->name);
  g_free (volume->device_file);
  g_free (volume->uuid);

  G_OBJECT_CLASS (gvfs_udisks2_volume_parent_class)->finalize (object);
}

static void
gvfs_udisks2_volume_class_init (GVfsUDisks2VolumeClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gvfs_udisks2_volume_finalize;
}

static void
gvfs_udisks2_volume_init (GVfsUDisks2Volume *volume)
{
}

static void
emit_changed (GVfsUDisks2Volume *volume)
{
  g_signal_emit_by_name (volume, "changed");
  g_signal_emit_by_name (volume->monitor, "volume_changed", volume);
}

static UDisksDrive *
get_udisks_drive (GVfsUDisks2Volume *volume)
{
  UDisksDrive *ret = NULL;
  UDisksClient *client;
  UDisksObject *object;

  client = gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor);
  object = (UDisksObject *) g_dbus_object_manager_get_object (udisks_client_get_object_manager (client),
                                                              udisks_block_get_drive (volume->block));
  if (object != NULL)
    {
      ret = udisks_object_peek_drive (object);
      g_object_unref (object);
    }
  return ret;
}

static gboolean
update_volume (GVfsUDisks2Volume *volume)
{
  gboolean changed;
  gboolean old_can_mount;
  gboolean old_should_automount;
  gchar *old_name;
  gchar *old_device_file;
  dev_t old_dev;
  GIcon *old_icon;
  UDisksDrive *udisks_drive;
  gchar *s;

  /* ---------------------------------------------------------------------------------------------------- */
  /* save old values */

  old_can_mount = volume->can_mount;
  old_should_automount = volume->should_automount;
  old_name = g_strdup (volume->name);
  old_device_file = g_strdup (volume->device_file);
  old_dev = volume->dev;
  old_icon = volume->icon != NULL ? g_object_ref (volume->icon) : NULL;

  /* ---------------------------------------------------------------------------------------------------- */
  /* reset */

  volume->can_mount = volume->should_automount = FALSE;
  g_free (volume->name); volume->name = NULL;
  g_free (volume->device_file); volume->device_file = NULL;
  volume->dev = 0;
  g_clear_object (&volume->icon);

  /* ---------------------------------------------------------------------------------------------------- */
  /* in with the new */

  volume->dev = makedev (udisks_block_get_major (volume->block), udisks_block_get_minor (volume->block));
  volume->device_file = udisks_block_dup_device (volume->block);

  if (strlen (udisks_block_get_id_label (volume->block)) > 0)
    {
      volume->name = g_strdup (udisks_block_get_id_label (volume->block));
    }
  else
    {
      s = g_format_size (udisks_block_get_size (volume->block));
      /* Translators: This is used for volume with no filesystem label.
       *              The first %s is the formatted size (e.g. "42.0 MB").
       */
      volume->name = g_strdup_printf (_("%s Volume"), s);
      g_free (s);
    }

  udisks_drive = get_udisks_drive (volume);
  if (udisks_drive != NULL)
    {
      gchar *drive_desc;
      GIcon *drive_icon;
      gchar *media_desc;
      GIcon *media_icon;
      udisks_util_get_drive_info (udisks_drive,
                                  NULL, /* drive_name */
                                  &drive_desc,
                                  &drive_icon,
                                  &media_desc,
                                  &media_icon);
      if (media_desc == NULL)
        {
          media_desc = drive_desc;
          drive_desc = NULL;
        }
      if (media_icon == NULL)
        {
          media_icon = drive_icon;
          drive_icon = NULL;
        }

      //volume->name = g_strdup (media_desc);
      volume->icon = media_icon != NULL ? g_object_ref (media_icon) : NULL;

      g_free (media_desc);
      if (media_icon != NULL)
        g_object_unref (media_icon);
    }
  else
    {
    }

  /* ---------------------------------------------------------------------------------------------------- */
  /* fallbacks */

  if (volume->name == NULL)
    {
      /* Translators: Name used for volume */
      volume->name = g_strdup (_("Volume"));
    }
  if (volume->icon == NULL)
    volume->icon = g_themed_icon_new ("drive-removable-media");

  /* ---------------------------------------------------------------------------------------------------- */
  /* compute whether something changed */

  changed = !((old_can_mount == volume->can_mount) &&
              (old_should_automount == volume->should_automount) &&
              (g_strcmp0 (old_name, volume->name) == 0) &&
              (g_strcmp0 (old_device_file, volume->device_file) == 0) &&
              (old_dev == volume->dev) &&
              g_icon_equal (old_icon, volume->icon)
              );

  /* ---------------------------------------------------------------------------------------------------- */
  /* free old values */

  g_free (old_name);
  g_free (old_device_file);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  return changed;
}

static void
on_block_changed (GObject    *object,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (user_data);
  if (update_volume (volume))
    emit_changed (volume);
}

GVfsUDisks2Volume *
gvfs_udisks2_volume_new (GVfsUDisks2VolumeMonitor   *monitor,
                         UDisksBlock                *block,
                         GVfsUDisks2Drive           *drive,
                         GFile                      *activation_root)
{
  GVfsUDisks2Volume *volume;

  volume = g_object_new (GVFS_TYPE_UDISKS2_VOLUME, NULL);
  volume->monitor = monitor;

  volume->block = g_object_ref (block);
  g_signal_connect (volume->block, "notify", G_CALLBACK (on_block_changed), volume);

  volume->activation_root = activation_root != NULL ? g_object_ref (activation_root) : NULL;

  volume->drive = drive;
  if (drive != NULL)
    gvfs_udisks2_drive_set_volume (drive, volume);

  update_volume (volume);

  return volume;
}

void
gvfs_udisks2_volume_removed (GVfsUDisks2Volume *volume)
{
#if 0
TODO
  if (volume->pending_mount_op != NULL)
    cancel_pending_mount_op (volume->pending_mount_op);
#endif

  if (volume->mount != NULL)
    {
      gvfs_udisks2_mount_unset_volume (volume->mount, volume);
      volume->mount = NULL;
    }

  if (volume->drive != NULL)
    {
      gvfs_udisks2_drive_unset_volume (volume->drive, volume);
      volume->drive = NULL;
    }
}

void
gvfs_udisks2_volume_set_mount (GVfsUDisks2Volume *volume,
                               GVfsUDisks2Mount  *mount)
{
  if (volume->mount != mount)
    {
      if (volume->mount != NULL)
        gvfs_udisks2_mount_unset_volume (volume->mount, volume);

      volume->mount = mount;

      emit_changed (volume);
    }
}

void
gvfs_udisks2_volume_unset_mount (GVfsUDisks2Volume *volume,
                                 GVfsUDisks2Mount  *mount)
{
  if (volume->mount == mount)
    {
      volume->mount = NULL;
      emit_changed (volume);
    }
}

void
gvfs_udisks2_volume_set_drive (GVfsUDisks2Volume *volume,
                               GVfsUDisks2Drive  *drive)
{
  if (volume->drive != drive)
    {
      if (volume->drive != NULL)
        gvfs_udisks2_drive_unset_volume (volume->drive, volume);
      volume->drive = drive;
      emit_changed (volume);
    }
}

void
gvfs_udisks2_volume_unset_drive (GVfsUDisks2Volume *volume,
                                 GVfsUDisks2Drive  *drive)
{
  if (volume->drive == drive)
    {
      volume->drive = NULL;
      emit_changed (volume);
    }
}

static GIcon *
gvfs_udisks2_volume_get_icon (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->icon != NULL ? g_object_ref (volume->icon) : NULL;
}

static char *
gvfs_udisks2_volume_get_name (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return g_strdup (volume->name);
}

static char *
gvfs_udisks2_volume_get_uuid (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return g_strdup (volume->uuid);
}

static gboolean
gvfs_udisks2_volume_can_mount (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->can_mount;
}

static gboolean
gvfs_udisks2_volume_can_eject (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  gboolean can_eject = FALSE;

  if (volume->drive != NULL)
    can_eject = g_drive_can_eject (G_DRIVE (volume->drive));
  return can_eject;
}

static gboolean
gvfs_udisks2_volume_should_automount (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->should_automount;
}

static GDrive *
gvfs_udisks2_volume_get_drive (GVolume *volume)
{
  GVfsUDisks2Volume *gdu_volume = GVFS_UDISKS2_VOLUME (volume);
  GDrive *drive = NULL;

  if (gdu_volume->drive != NULL)
    drive = g_object_ref (gdu_volume->drive);
  return drive;
}

static GMount *
gvfs_udisks2_volume_get_mount (GVolume *volume)
{
  GVfsUDisks2Volume *gdu_volume = GVFS_UDISKS2_VOLUME (volume);
  GMount *mount = NULL;

  if (gdu_volume->mount != NULL)
    mount = g_object_ref (gdu_volume->mount);
  return mount;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
gvfs_udisks2_volume_get_identifier (GVolume      *_volume,
                                    const gchar  *kind)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  const gchar *label;
  const gchar *uuid;
  gchar *ret = NULL;

  label = udisks_block_get_id_label (volume->block);
  uuid = udisks_block_get_id_uuid (volume->block);

  if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) == 0)
    ret = g_strdup (volume->device_file);
  else if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_LABEL) == 0)
    ret = strlen (label) > 0 ? g_strdup (label) : NULL;
  else if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UUID) == 0)
    ret = strlen (uuid) > 0 ? g_strdup (uuid) : NULL;

  return ret;
}

static gchar **
gvfs_udisks2_volume_enumerate_identifiers (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  const gchar *label;
  const gchar *uuid;
  GPtrArray *p;

  label = udisks_block_get_id_label (volume->block);
  uuid = udisks_block_get_id_uuid (volume->block);

  p = g_ptr_array_new ();
  g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
  if (strlen (label) > 0)
    g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_LABEL));
  if (strlen (uuid) > 0)
    g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_UUID));

  g_ptr_array_add (p, NULL);
  return (gchar **) g_ptr_array_free (p, FALSE);
}

static GFile *
gvfs_udisks2_volume_get_activation_root (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->activation_root != NULL ? g_object_ref (volume->activation_root) : NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_udisks2_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = gvfs_udisks2_volume_get_name;
  iface->get_icon = gvfs_udisks2_volume_get_icon;
  iface->get_uuid = gvfs_udisks2_volume_get_uuid;
  iface->get_drive = gvfs_udisks2_volume_get_drive;
  iface->get_mount = gvfs_udisks2_volume_get_mount;
  iface->can_mount = gvfs_udisks2_volume_can_mount;
  iface->can_eject = gvfs_udisks2_volume_can_eject;
  iface->should_automount = gvfs_udisks2_volume_should_automount;
  iface->get_activation_root = gvfs_udisks2_volume_get_activation_root;
  iface->enumerate_identifiers = gvfs_udisks2_volume_enumerate_identifiers;
  iface->get_identifier = gvfs_udisks2_volume_get_identifier;

#if 0
  iface->mount_fn = gvfs_udisks2_volume_mount;
  iface->mount_finish = gvfs_udisks2_volume_mount_finish;
  iface->eject = gvfs_udisks2_volume_eject;
  iface->eject_finish = gvfs_udisks2_volume_eject_finish;
  iface->eject_with_operation = gvfs_udisks2_volume_eject_with_operation;
  iface->eject_with_operation_finish = gvfs_udisks2_volume_eject_with_operation_finish;
#endif
}

/* ---------------------------------------------------------------------------------------------------- */

UDisksBlock *
gvfs_udisks2_volume_get_block (GVfsUDisks2Volume *volume)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME (volume), NULL);
  return volume->block;
}

dev_t
gvfs_udisks2_volume_get_dev (GVfsUDisks2Volume *volume)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME (volume), 0);
  return volume->dev;
}

gboolean
gvfs_udisks2_volume_has_uuid (GVfsUDisks2Volume *volume,
                              const gchar       *uuid)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME (volume), FALSE);
  return g_strcmp0 (volume->uuid, uuid) == 0;
}
