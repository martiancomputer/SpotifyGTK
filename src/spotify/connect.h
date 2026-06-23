/*
 * connect.h — Spotify Connect device registration.
 *
 * Registers SpotifyGTK as a controllable playback device (so it shows
 * up in the device picker on phones/other clients, and so the Web API
 * player endpoints in api.c can target it). This is a thin wrapper
 * over Mercury subscription + a device-state announcement message.
 */

#pragma once

#include <glib-object.h>
#include "mercury.h"

G_BEGIN_DECLS

#define SPOTIFYGTK_TYPE_CONNECT_DEVICE (spotifygtk_connect_device_get_type ())
G_DECLARE_FINAL_TYPE (SpotifyConnectDevice, spotifygtk_connect_device,
                      SPOTIFYGTK, CONNECT_DEVICE, GObject)

SpotifyConnectDevice *spotifygtk_connect_device_new (SpotifyMercury *mercury, const gchar *device_name);

void spotifygtk_connect_device_register   (SpotifyConnectDevice *self);
void spotifygtk_connect_device_unregister (SpotifyConnectDevice *self);

/* Signal: "remote-command" (gchar *command_json) — fired when another
 * client (phone, web player) sends a play/pause/seek/transfer command
 * to this device via Connect. */

G_END_DECLS
