#include "playlist_create_dialog.h"

#include <string.h>

struct _SpotifyGtkPlaylistCreateDialog {
  AdwAlertDialog parent_instance;
  GtkStack *steps;
  GtkCheckButton *spotify;
  GtkEntry *name;
  GtkLabel *error;
  GtkButton *back;
  GtkButton *next;
  gboolean naming;
  gboolean busy;
  gboolean closed;
  gboolean spotify_available;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkPlaylistCreateDialog, spotifygtk_playlist_create_dialog,
                     ADW_TYPE_ALERT_DIALOG)

enum { CREATE_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
update_actions (SpotifyGtkPlaylistCreateDialog *self)
{
  g_autofree gchar *name = g_strstrip (g_strdup (
    gtk_editable_get_text (GTK_EDITABLE (self->name))));
  gboolean valid = self->naming ? (*name && strlen (name) <= 256) :
    (!gtk_check_button_get_active (self->spotify) || self->spotify_available);
  gtk_widget_set_sensitive (GTK_WIDGET (self->next), valid && !self->busy);
  gtk_widget_set_sensitive (GTK_WIDGET (self->back), !self->busy);
  gtk_widget_set_sensitive (GTK_WIDGET (self->steps), !self->busy);
}

static void
name_changed (GtkEditable *entry, SpotifyGtkPlaylistCreateDialog *self)
{
  g_autofree gchar *name = g_strstrip (g_strdup (gtk_editable_get_text (entry)));
  gboolean too_long = strlen (name) > 256;
  if (too_long)
    gtk_label_set_text (self->error, "The playlist name must be no more than 256 bytes.");
  gtk_widget_set_visible (GTK_WIDGET (self->error), too_long);
  update_actions (self);
}

static void
destination_changed (GtkCheckButton *button, SpotifyGtkPlaylistCreateDialog *self)
{
  (void) button;
  update_actions (self);
}

static void
set_step (SpotifyGtkPlaylistCreateDialog *self, gboolean naming)
{
  self->naming = naming;
  gtk_stack_set_visible_child_name (self->steps, naming ? "name" : "destination");
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (self), naming
    ? (gtk_check_button_get_active (self->spotify)
        ? "This playlist will sync to Spotify."
        : "This playlist stays on this device.")
    : "Where should this playlist live?");
  gtk_button_set_label (self->back, naming ? "Back" : "Cancel");
  gtk_button_set_label (self->next, naming ? "Create" : "Next");
  gtk_widget_set_visible (GTK_WIDGET (self->error), FALSE);
  update_actions (self);
  adw_dialog_set_focus (ADW_DIALOG (self), naming ? GTK_WIDGET (self->name)
                                                : GTK_WIDGET (self->next));
}

static void
go_back (GtkButton *button, SpotifyGtkPlaylistCreateDialog *self)
{
  (void) button;
  if (self->busy || self->closed) return;
  if (self->naming)
    set_step (self, FALSE);
  else
    adw_dialog_close (ADW_DIALOG (self));
}

static void
go_next (GtkButton *button, SpotifyGtkPlaylistCreateDialog *self)
{
  (void) button;
  if (self->busy || self->closed ||
      !gtk_widget_get_sensitive (GTK_WIDGET (self->next))) return;
  if (!self->naming) {
    set_step (self, TRUE);
    return;
  }
  g_autofree gchar *name = g_strstrip (g_strdup (
    gtk_editable_get_text (GTK_EDITABLE (self->name))));
  if (!*name || strlen (name) > 256) return;
  self->busy = TRUE;
  update_actions (self);
  gtk_button_set_label (self->next, "Creating…");
  g_signal_emit (self, signals[CREATE_REQUESTED], 0,
                 gtk_check_button_get_active (self->spotify), name);
}

static void
dialog_closed (AdwDialog *dialog, gpointer data)
{
  SPOTIFYGTK_PLAYLIST_CREATE_DIALOG (dialog)->closed = TRUE;
  (void) data;
}

static void
name_activated (GtkEntry *entry, SpotifyGtkPlaylistCreateDialog *self)
{
  (void) entry;
  /* AlertDialog manages its own default response internally. Our two-step
   * actions do not close the dialog like responses do, so handle Enter here. */
  go_next (self->next, self);
}

static void
spotifygtk_playlist_create_dialog_class_init (SpotifyGtkPlaylistCreateDialogClass *klass)
{
  signals[CREATE_REQUESTED] = g_signal_new ("create-requested",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_BOOLEAN, G_TYPE_STRING);
}

static GtkWidget *
destination_row (const gchar *title, const gchar *detail, GtkCheckButton **check)
{
  GtkWidget *row = gtk_check_button_new ();
  GtkWidget *labels = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
  GtkWidget *heading = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (heading), 0);
  gtk_box_append (GTK_BOX (labels), heading);
  GtkWidget *subtitle = gtk_label_new (detail);
  gtk_label_set_xalign (GTK_LABEL (subtitle), 0);
  gtk_label_set_wrap (GTK_LABEL (subtitle), TRUE);
  gtk_widget_add_css_class (subtitle, "caption");
  gtk_widget_add_css_class (subtitle, "dim-label");
  gtk_box_append (GTK_BOX (labels), subtitle);
  gtk_check_button_set_child (GTK_CHECK_BUTTON (row), labels);
  gtk_widget_add_css_class (row, "playlist-destination");
  *check = GTK_CHECK_BUTTON (row);
  return row;
}

static void
spotifygtk_playlist_create_dialog_init (SpotifyGtkPlaylistCreateDialog *self)
{
  adw_alert_dialog_set_heading (ADW_ALERT_DIALOG (self), "Create playlist");
  adw_dialog_set_title (ADW_DIALOG (self), "Create playlist");
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top (content, 12);
  self->steps = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->steps, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (self->steps, 120);
  gtk_stack_set_vhomogeneous (self->steps, FALSE);
  GtkWidget *choices = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_append (GTK_BOX (choices), destination_row ("Spotify",
    "Syncs to your Spotify account", &self->spotify));
  GtkCheckButton *device;
  gtk_box_append (GTK_BOX (choices), destination_row ("Device playlist",
    "Saved only on this device", &device));
  gtk_check_button_set_group (device, self->spotify);
  self->name = GTK_ENTRY (gtk_entry_new ());
  gtk_entry_set_placeholder_text (self->name, "Playlist name");
  gtk_entry_set_max_length (self->name, 256);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->name),
    GTK_ACCESSIBLE_PROPERTY_LABEL, "Playlist name", -1);
  gtk_stack_add_named (self->steps, choices, "destination");
  gtk_stack_add_named (self->steps, GTK_WIDGET (self->name), "name");
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->steps));
  self->error = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->error, TRUE);
  gtk_label_set_max_width_chars (self->error, 36);
  gtk_widget_add_css_class (GTK_WIDGET (self->error), "error");
  gtk_widget_add_css_class (GTK_WIDGET (self->error), "caption");
  gtk_widget_set_visible (GTK_WIDGET (self->error), FALSE);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->error));
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous (GTK_BOX (actions), TRUE);
  gtk_widget_add_css_class (actions, "playlist-wizard-actions");
  self->back = GTK_BUTTON (gtk_button_new_with_label ("Cancel"));
  self->next = GTK_BUTTON (gtk_button_new_with_label ("Next"));
  gtk_box_append (GTK_BOX (actions), GTK_WIDGET (self->back));
  gtk_box_append (GTK_BOX (actions), GTK_WIDGET (self->next));
  gtk_widget_set_margin_top (actions, 8);
  gtk_box_append (GTK_BOX (content), actions);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (self), content);
  adw_dialog_set_default_widget (ADW_DIALOG (self), GTK_WIDGET (self->next));
  g_signal_connect (self->name, "changed", G_CALLBACK (name_changed), self);
  g_signal_connect (self->name, "activate", G_CALLBACK (name_activated), self);
  g_signal_connect (self->spotify, "toggled", G_CALLBACK (destination_changed), self);
  g_signal_connect (self->back, "clicked", G_CALLBACK (go_back), self);
  g_signal_connect (self->next, "clicked", G_CALLBACK (go_next), self);
  g_signal_connect (self, "closed", G_CALLBACK (dialog_closed), NULL);
}

SpotifyGtkPlaylistCreateDialog *
spotifygtk_playlist_create_dialog_new (gboolean spotify_available)
{
  SpotifyGtkPlaylistCreateDialog *self = g_object_new (
    SPOTIFYGTK_TYPE_PLAYLIST_CREATE_DIALOG, NULL);
  self->spotify_available = spotify_available;
  gtk_widget_set_sensitive (GTK_WIDGET (self->spotify), spotify_available);
  if (!spotify_available)
    gtk_widget_set_tooltip_text (GTK_WIDGET (self->spotify), "Sign in to create a Spotify playlist");
  gtk_check_button_set_active (spotify_available ? self->spotify :
    GTK_CHECK_BUTTON (gtk_widget_get_next_sibling (GTK_WIDGET (self->spotify))), TRUE);
  set_step (self, FALSE);
  return self;
}

void
spotifygtk_playlist_create_dialog_complete (SpotifyGtkPlaylistCreateDialog *self,
                                          const gchar *error)
{
  g_return_if_fail (SPOTIFYGTK_IS_PLAYLIST_CREATE_DIALOG (self));
  if (self->closed) return;
  self->busy = FALSE;
  if (!error) {
    adw_dialog_close (ADW_DIALOG (self));
    return;
  }
  gtk_label_set_text (self->error, error);
  gtk_widget_set_visible (GTK_WIDGET (self->error), TRUE);
  gtk_button_set_label (self->next, "Create");
  update_actions (self);
}
