#include "artwork_viewer.h"
#include "cover_loader.h"
#include "dialog_host.h"

/* Three times the context-page cover, bounded by the host and source image.
 * Fetch/decode independently of the 320px hero; never enlarge its thumbnail. */
#define ARTWORK_VIEWER_MAX_SIZE 798
#define ARTWORK_VIEWER_MAX_DECODE 2048

typedef struct {
  AdwDialog parent_instance;
  GtkPicture *picture;
  GtkLabel *status;
  GCancellable *request;
  gint size;
  gboolean closed;
} SpotifyGtkArtworkViewer;

typedef AdwDialogClass SpotifyGtkArtworkViewerClass;
G_DEFINE_TYPE (SpotifyGtkArtworkViewer, spotifygtk_artwork_viewer, ADW_TYPE_DIALOG)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SpotifyGtkArtworkViewer, g_object_unref)

static void
release_artwork (SpotifyGtkArtworkViewer *self)
{
  self->closed = TRUE;
  if (self->request) g_cancellable_cancel (self->request);
  g_clear_object (&self->request);
  /* closed is emitted at the START of AdwDialog's closing animation. Clear
   * pixels now, not when the host eventually removes the dialog widget. */
  if (self->picture) gtk_picture_set_paintable (self->picture, NULL);
}

static void
viewer_closed (AdwDialog *dialog, gpointer data)
{
  release_artwork ((SpotifyGtkArtworkViewer *) dialog);
  (void) data;
}

static void
viewer_dispose (GObject *object)
{
  SpotifyGtkArtworkViewer *self = (SpotifyGtkArtworkViewer *) object;
  release_artwork (self);
  self->picture = NULL;
  self->status = NULL;
  G_OBJECT_CLASS (spotifygtk_artwork_viewer_parent_class)->dispose (object);
}

static void
spotifygtk_artwork_viewer_class_init (SpotifyGtkArtworkViewerClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = viewer_dispose;
}

static void
spotifygtk_artwork_viewer_init (SpotifyGtkArtworkViewer *self)
{
  spotifygtk_dialog_prepare (ADW_DIALOG (self));
  gtk_widget_add_css_class (GTK_WIDGET (self), "artwork-viewer");
  GtkWidget *body = gtk_overlay_new ();
  self->picture = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_can_shrink (self->picture, TRUE);
  gtk_picture_set_content_fit (self->picture, GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_focusable (GTK_WIDGET (self->picture), TRUE);
  gtk_overlay_set_child (GTK_OVERLAY (body), GTK_WIDGET (self->picture));
  self->status = GTK_LABEL (gtk_label_new ("Loading artwork…"));
  gtk_label_set_wrap (self->status, TRUE);
  gtk_label_set_justify (self->status, GTK_JUSTIFY_CENTER);
  gtk_widget_set_halign (GTK_WIDGET (self->status), GTK_ALIGN_CENTER);
  gtk_widget_set_valign (GTK_WIDGET (self->status), GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay (GTK_OVERLAY (body), GTK_WIDGET (self->status));
  adw_dialog_set_child (ADW_DIALOG (self), body);
  adw_dialog_set_focus (ADW_DIALOG (self), GTK_WIDGET (self->picture));
  g_signal_connect (self, "closed", G_CALLBACK (viewer_closed), NULL);
}

static void
viewer_loaded (GdkTexture *texture, gpointer data)
{
  SpotifyGtkArtworkViewer *self = data;
  if (self->closed) return;
  if (!texture) {
    gtk_label_set_text (self->status, "Artwork could not be loaded.\nClick outside or press Escape to close.");
    return;
  }
  gint scale = MAX (1, gtk_widget_get_scale_factor (GTK_WIDGET (self)));
  gint width = gdk_texture_get_width (texture);
  gint height = gdk_texture_get_height (texture);
  gdouble factor = MIN (1.0 / scale, (gdouble) self->size / MAX (width, height));
  adw_dialog_set_content_width (ADW_DIALOG (self), MAX (1, (gint) (width * factor)));
  adw_dialog_set_content_height (ADW_DIALOG (self), MAX (1, (gint) (height * factor)));
  gtk_widget_set_visible (GTK_WIDGET (self->status), FALSE);
  gtk_picture_set_paintable (self->picture, GDK_PAINTABLE (texture));
}

void
spotifygtk_artwork_viewer_present (GtkWidget *parent, const gchar *cover_id,
                                  const gchar *title)
{
  g_return_if_fail (GTK_IS_WIDGET (parent));
  if (!cover_id || !*cover_id) return;
  GtkRoot *root = gtk_widget_get_root (parent);
  if (!ADW_IS_APPLICATION_WINDOW (root)) return;
  AdwDialog *visible = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (root));
  /* Never stack full-resolution viewers, even on a double click. */
  if (visible) return;
  g_autoptr(SpotifyGtkArtworkViewer) self = g_object_ref_sink (
    g_object_new (spotifygtk_artwork_viewer_get_type (), NULL));
  self->size = CLAMP (MIN (gtk_widget_get_width (GTK_WIDGET (root)),
                          gtk_widget_get_height (GTK_WIDGET (root))) - 96,
                      1, ARTWORK_VIEWER_MAX_SIZE);
  adw_dialog_set_content_width (ADW_DIALOG (self), self->size);
  adw_dialog_set_content_height (ADW_DIALOG (self), self->size);
  adw_dialog_set_title (ADW_DIALOG (self), title && *title ? title : "Artwork");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->picture),
    GTK_ACCESSIBLE_PROPERTY_LABEL, title && *title ? title : "Artwork", -1);
  self->request = g_cancellable_new ();
  adw_dialog_present (ADW_DIALOG (self), parent);
  /* A cancelled request suppresses delivery; it does not own the viewer.
   * Dispose also cancels, so a late completion cannot access a dead widget. */
  spotifygtk_cover_load (cover_id, MIN (ARTWORK_VIEWER_MAX_DECODE,
    self->size * MAX (1, gtk_widget_get_scale_factor (GTK_WIDGET (root)))),
    self->request, viewer_loaded, self);
}
