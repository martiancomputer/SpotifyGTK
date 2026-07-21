/*
 * home_page.c — Home page implementation.
 */

#include "home_page.h"

struct _SpotifyGtkHomePage {
  GtkBox parent_instance;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkHomePage, spotifygtk_home_page, GTK_TYPE_BOX)

static void
spotifygtk_home_page_class_init (SpotifyGtkHomePageClass *klass)
{
  (void) klass;
}

static GtkWidget *
build_pending_section (const gchar *heading, const gchar *detail)
{
  GtkWidget *section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

  GtkWidget *label = gtk_label_new (heading);
  gtk_widget_add_css_class (label, "normal-text");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (section), label);

  GtkWidget *note = gtk_label_new (detail);
  gtk_widget_add_css_class (note, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (note), 0.0);
  gtk_label_set_wrap (GTK_LABEL (note), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (note), 70);
  gtk_box_append (GTK_BOX (section), note);

  return section;
}

static void
spotifygtk_home_page_init (SpotifyGtkHomePage *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing (GTK_BOX (self), 20);
  gtk_widget_set_margin_start (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_end (GTK_WIDGET (self), 35);
  gtk_widget_set_margin_top (GTK_WIDGET (self), 24);
  gtk_widget_set_margin_bottom (GTK_WIDGET (self), 24);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

  GtkWidget *title = gtk_label_new ("Home");
  gtk_widget_add_css_class (title, "title-text");
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (self), title);

  GtkWidget *intro = gtk_label_new ("Search and Liked Songs run on the native "
                                    "protocol stack. These two sections do not yet.");
  gtk_widget_add_css_class (intro, "dim-text");
  gtk_label_set_xalign (GTK_LABEL (intro), 0.0);
  gtk_label_set_wrap (GTK_LABEL (intro), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (intro), 70);
  gtk_box_append (GTK_BOX (self), intro);

  gtk_box_append (GTK_BOX (self),
    build_pending_section ("Continue Listening",
      "Needs the playlist rootlist, which comes back as playlist4_external "
      "protobuf rather than the JSON the rest of the catalog uses."));

  gtk_box_append (GTK_BOX (self),
    build_pending_section ("Recently Played",
      "No spclient endpoint for listening history is known — librespot "
      "exposes none, so there is nothing to port here yet."));
}

SpotifyGtkHomePage *
spotifygtk_home_page_new (void)
{
  return g_object_new (SPOTIFYGTK_TYPE_HOME_PAGE, NULL);
}
