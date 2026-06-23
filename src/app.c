#include "config.h"
#include "app.h"
#include "auth.h"
#include "ui/window.h"

struct _SpotifyGtkApp {
  AdwApplication parent_instance;

  SpotifyAuth   *auth;
  SpotifyWindow *window;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkApp, spotifygtk_app, ADW_TYPE_APPLICATION)

static void
on_auth_completed (SpotifyAuth *auth,
                   gboolean     success,
                   gpointer     user_data)
{
  SpotifyGtkApp *self = SPOTIFYGTK_APP (user_data);

  if (!success) {
    g_warning ("Authentication failed");
    g_application_quit (G_APPLICATION (self));
    return;
  }

  spotifygtk_window_set_authenticated (self->window, TRUE);
}

static void
spotifygtk_app_activate (GApplication *app)
{
  SpotifyGtkApp *self = SPOTIFYGTK_APP (app);

  if (!self->window) {
    self->window = spotifygtk_window_new (GTK_APPLICATION (app));
    self->auth   = spotifygtk_auth_new ();

    g_signal_connect (self->auth, "completed",
                      G_CALLBACK (on_auth_completed), self);
  }

  gtk_window_present (GTK_WINDOW (self->window));

  if (!spotifygtk_auth_has_valid_token (self->auth))
    spotifygtk_auth_begin (self->auth);
}

static void
spotifygtk_app_startup (GApplication *app)
{
  G_APPLICATION_CLASS (spotifygtk_app_parent_class)->startup (app);
  adw_init ();
}

static void
spotifygtk_app_dispose (GObject *object)
{
  SpotifyGtkApp *self = SPOTIFYGTK_APP (object);
  g_clear_object (&self->auth);
  G_OBJECT_CLASS (spotifygtk_app_parent_class)->dispose (object);
}

static void
spotifygtk_app_class_init (SpotifyGtkAppClass *klass)
{
  GObjectClass      *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class    = G_APPLICATION_CLASS (klass);

  object_class->dispose  = spotifygtk_app_dispose;
  app_class->activate    = spotifygtk_app_activate;
  app_class->startup     = spotifygtk_app_startup;
}

static void
spotifygtk_app_init (SpotifyGtkApp *self)
{
  (void) self;
}

SpotifyGtkApp *
spotifygtk_app_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_APP,
                       "application-id", APP_ID,
                       "flags",          G_APPLICATION_DEFAULT_FLAGS,
                       NULL);
}
