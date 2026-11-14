#include "playlist_create_dialog.h"

#include <string.h>
#include <math.h>

struct _SpotifyGtkPlaylistCreateDialog {
  AdwAlertDialog parent_instance;
  GtkStack *steps;
  GtkCheckButton *spotify;
  GtkEntry *name;
  GtkLabel *error;
  GtkButton *back;
  GtkButton *next;
  gboolean naming;
  guint step;
  GtkWidget *crop;
  GtkScale *zoom;
  GtkWidget *skip;
  GtkWidget *choose;
  GdkPixbuf *image;
  cairo_surface_t *image_surface;
  GBytes *jpeg;
  GCancellable *image_cancel;
  double crop_x, crop_y, drag_x, drag_y;
  gboolean edit_artwork;
  gboolean busy;
  gboolean closed;
  gboolean spotify_available;
};

G_DEFINE_FINAL_TYPE (SpotifyGtkPlaylistCreateDialog, spotifygtk_playlist_create_dialog,
                     ADW_TYPE_ALERT_DIALOG)

enum { CREATE_REQUESTED, ARTWORK_REQUESTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
update_actions (SpotifyGtkPlaylistCreateDialog *self)
{
  g_autofree gchar *name = g_strstrip (g_strdup (
    gtk_editable_get_text (GTK_EDITABLE (self->name))));
  gboolean valid = self->step == 0
    ? (!gtk_check_button_get_active (self->spotify) || self->spotify_available)
    : self->step == 1 ? (*name && strlen (name) <= 256) : self->image != NULL;
  gtk_widget_set_sensitive (GTK_WIDGET (self->next), valid && !self->busy);
  gtk_widget_set_sensitive (GTK_WIDGET (self->back), !self->busy);
  gtk_widget_set_sensitive (self->skip, !self->busy);
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
set_step (SpotifyGtkPlaylistCreateDialog *self, guint step)
{
  self->step = step; self->naming = step == 1;
  const gchar *steps[] = { "destination", "name", "image", "crop" };
  const gchar *bodies[] = { "Where should this playlist live?", "Give your playlist a name.",
    "Choose a JPEG or PNG, or skip the image.", "Drag to position. Zoom to crop." };
  gtk_stack_set_visible_child_name (self->steps, steps[step]);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (self), self->edit_artwork && step == 2
    ? "Choose a JPEG or PNG image." : bodies[step]);
  gboolean created = g_object_get_data (G_OBJECT (self), "created-playlist-uri") != NULL;
  gtk_button_set_label (self->back, step == 0 || ((self->edit_artwork || created) && step == 2) ? "Cancel" : "Back");
  gtk_button_set_label (self->next, step == 3 ? (self->edit_artwork ? "Save image" : "Create") : "Next");
  gtk_widget_set_visible (GTK_WIDGET (self->next), step != 2);
  gtk_widget_set_visible (self->skip, step == 2 && !self->edit_artwork);
  gtk_button_set_label (GTK_BUTTON (self->skip), "Skip image");
  gtk_widget_set_visible (GTK_WIDGET (self->error), FALSE);
  update_actions (self);
  GtkWidget *action = step == 2 ? self->choose : GTK_WIDGET (self->next);
  adw_dialog_set_default_widget (ADW_DIALOG (self), action);
  adw_dialog_set_focus (ADW_DIALOG (self), step == 1 ? GTK_WIDGET (self->name) : action);
}

static void
go_back (GtkButton *button, SpotifyGtkPlaylistCreateDialog *self)
{
  (void) button;
  if (self->busy || self->closed) return;
  gboolean created = g_object_get_data (G_OBJECT (self), "created-playlist-uri") != NULL;
  if (self->step > 0 && !((self->edit_artwork || created) && self->step == 2))
    set_step (self, self->step - 1);
  else
    adw_dialog_close (ADW_DIALOG (self));
}

/* Image work is bounded and runs on GTask workers. The editor retains one
 * downsampled source; saved artwork is an opaque square JPEG without EXIF. */
#define ART_INPUT_LIMIT (16 * 1024 * 1024)
#define ART_OUTPUT_PX 640
#define ART_OUTPUT_LIMIT (192 * 1024)

typedef struct { gboolean valid; } ImageDimensions;
typedef struct { GdkPixbuf *pixbuf; cairo_surface_t *surface; } LoadedImage;
static void loaded_image_free (gpointer data)
{
  LoadedImage *image = data;
  g_object_unref (image->pixbuf); cairo_surface_destroy (image->surface); g_free (image);
}

static void
image_size_prepared (GdkPixbufLoader *loader, gint width, gint height, gpointer data)
{
  ImageDimensions *dimensions = data;
  dimensions->valid = width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
    (guint64) width * height <= 32 * 1024 * 1024;
  double factor = MIN (1.0, 2048.0 / MAX (width, height));
  gdk_pixbuf_loader_set_size (loader, dimensions->valid ? MAX (1, (gint) (width * factor)) : 1,
    dimensions->valid ? MAX (1, (gint) (height * factor)) : 1);
}

static void
load_image_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  GFile *file = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileInputStream) stream = g_file_read (file, cancel, &error);
  if (!stream) { g_task_return_error (task, g_steal_pointer (&error)); return; }
  g_autoptr(GByteArray) bytes = g_byte_array_new ();
  guint8 chunk[16384];
  gssize n;
  while ((n = g_input_stream_read (G_INPUT_STREAM (stream), chunk, sizeof chunk, cancel, &error)) > 0) {
    if ((gsize) n > ART_INPUT_LIMIT - bytes->len) {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Choose an image smaller than 16 MB."); return;
    }
    g_byte_array_append (bytes, chunk, n);
  }
  if (n < 0) { g_task_return_error (task, g_steal_pointer (&error)); return; }
  g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new ();
  ImageDimensions dimensions = { FALSE };
  g_signal_connect (loader, "size-prepared", G_CALLBACK (image_size_prepared), &dimensions);
  gboolean decoded = gdk_pixbuf_loader_write (loader, bytes->data, bytes->len, &error);
  if (!gdk_pixbuf_loader_close (loader, decoded ? &error : NULL)) decoded = FALSE;
  GdkPixbuf *pixbuf = decoded ? gdk_pixbuf_loader_get_pixbuf (loader) : NULL;
  if (!dimensions.valid || !pixbuf) {
    g_clear_error (&error);
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
      "Choose a JPEG or PNG image up to 8192 pixels per side and 32 megapixels."); return;
  }
  LoadedImage *image = g_new0 (LoadedImage, 1);
  image->pixbuf = gdk_pixbuf_apply_embedded_orientation (pixbuf);
  gint w = gdk_pixbuf_get_width (image->pixbuf), h = gdk_pixbuf_get_height (image->pixbuf);
  image->surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  guint8 *dest = cairo_image_surface_get_data (image->surface);
  const guint8 *pixels = gdk_pixbuf_read_pixels (image->pixbuf);
  gint channels = gdk_pixbuf_get_n_channels (image->pixbuf);
  for (gint y = 0; y < h; y++) {
    guint32 *row = (guint32 *) (dest + y * cairo_image_surface_get_stride (image->surface));
    const guint8 *src = pixels + y * gdk_pixbuf_get_rowstride (image->pixbuf);
    for (gint x = 0; x < w; x++, src += channels) {
      guint a = channels == 4 ? src[3] : 255;
      row[x] = (a << 24) | (((src[0] * a + 127) / 255) << 16) |
        (((src[1] * a + 127) / 255) << 8) | ((src[2] * a + 127) / 255);
    }
  }
  cairo_surface_mark_dirty (image->surface);
  g_task_return_pointer (task, image, loaded_image_free);
  (void) source;
}

static void
crop_geometry (SpotifyGtkPlaylistCreateDialog *self, double *side, double *x, double *y)
{
  double width = gdk_pixbuf_get_width (self->image), height = gdk_pixbuf_get_height (self->image);
  *side = MIN (width, height) / gtk_range_get_value (GTK_RANGE (self->zoom));
  self->crop_x = CLAMP (self->crop_x, *side / 2, width - *side / 2);
  self->crop_y = CLAMP (self->crop_y, *side / 2, height - *side / 2);
  *x = self->crop_x - *side / 2; *y = self->crop_y - *side / 2;
}

static void
crop_draw (GtkDrawingArea *area, cairo_t *cr, gint width, gint height, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  if (!self->image) return;
  double side, x, y, size = MIN (width, height);
  crop_geometry (self, &side, &x, &y);
  cairo_save (cr);
  cairo_rectangle (cr, 0, 0, size, size); cairo_clip (cr);
  cairo_set_source_rgb (cr, 1, 1, 1); cairo_paint (cr);
  cairo_scale (cr, size / side, size / side);
  cairo_set_source_surface (cr, self->image_surface, -x, -y); cairo_paint (cr);
  cairo_restore (cr);
  /* Rule-of-thirds guides stay in display coordinates at every zoom level. */
  for (guint pass = 0; pass < 2; pass++) {
    cairo_set_source_rgba (cr, pass ? 1 : 0, pass ? 1 : 0, pass ? 1 : 0, pass ? .75 : .55);
    cairo_set_line_width (cr, pass ? 1 : 3);
    for (guint i = 1; i < 3; i++) {
      double pos = size * i / 3;
      cairo_move_to (cr, pos, 0); cairo_line_to (cr, pos, size);
      cairo_move_to (cr, 0, pos); cairo_line_to (cr, size, pos);
    }
    cairo_stroke (cr);
  }
  (void) area;
}

static void
crop_changed (GtkRange *range, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  g_clear_pointer (&self->jpeg, g_bytes_unref);
  gtk_widget_queue_draw (self->crop);
  (void) range;
}

static void
crop_drag_begin (GtkGestureDrag *gesture, double x, double y, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  /* Claim the press before the dialog's window handle sees it. Waiting for
   * motion would also let its titlebar double-click gesture run first. */
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  self->drag_x = self->crop_x; self->drag_y = self->crop_y;
  gtk_widget_set_cursor_from_name (self->crop, "grabbing");
  (void) x; (void) y;
}

static void
crop_drag_end (GtkGestureDrag *gesture, double x, double y, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  gtk_widget_set_cursor_from_name (self->crop, "grab");
  (void) gesture; (void) x; (void) y;
}

static void
crop_drag_cancel (GtkGesture *gesture, GdkEventSequence *sequence, gpointer data)
{
  crop_drag_end (GTK_GESTURE_DRAG (gesture), 0, 0, data);
  (void) sequence;
}

static void
crop_drag_update (GtkGestureDrag *gesture, double x, double y, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  if (!self->image) return;
  double side = MIN (gdk_pixbuf_get_width (self->image), gdk_pixbuf_get_height (self->image)) /
    gtk_range_get_value (GTK_RANGE (self->zoom));
  double scale = side / MAX (1, gtk_widget_get_width (self->crop));
  self->crop_x = self->drag_x - x * scale; self->crop_y = self->drag_y - y * scale;
  crop_changed (NULL, self);
  (void) gesture;
}

static void
image_loaded (GObject *source, GAsyncResult *result, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  g_autoptr(GError) error = NULL;
  LoadedImage *loaded = g_task_propagate_pointer (G_TASK (result), &error);
  GdkPixbuf *image = loaded ? loaded->pixbuf : NULL;
  self->busy = FALSE;
  if (!self->closed) {
    if (image) {
      g_set_object (&self->image, image);
      g_clear_pointer (&self->image_surface, cairo_surface_destroy);
      self->image_surface = cairo_surface_reference (loaded->surface);
      g_clear_pointer (&self->jpeg, g_bytes_unref);
      self->crop_x = gdk_pixbuf_get_width (image) / 2.0;
      self->crop_y = gdk_pixbuf_get_height (image) / 2.0;
      gtk_range_set_value (GTK_RANGE (self->zoom), 1);
      gtk_widget_queue_draw (self->crop);
      set_step (self, 3);
    } else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      gtk_label_set_text (self->error, error->message);
      gtk_widget_set_visible (GTK_WIDGET (self->error), TRUE);
    }
    update_actions (self);
  }
  if (loaded) loaded_image_free (loaded);
  g_object_unref (self);
  (void) source;
}

static void
file_selected (GObject *source, GAsyncResult *result, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);
  self->busy = FALSE;
  if (!self->closed) update_actions (self);
  if (file && !self->closed) {
    self->busy = TRUE; update_actions (self);
    GTask *task = g_task_new (NULL, self->image_cancel, image_loaded, g_object_ref (self));
    g_task_set_task_data (task, g_object_ref (file), g_object_unref);
    g_task_run_in_thread (task, load_image_worker); g_object_unref (task);
  }
  g_object_unref (self);
}

static void
choose_image (GtkButton *button, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  if (self->busy || self->closed) return;
  self->busy = TRUE; update_actions (self);
  g_autoptr(GtkFileDialog) chooser = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (chooser, "Choose playlist image");
  g_autoptr(GtkFileFilter) filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, "Images (JPEG, PNG)");
  gtk_file_filter_add_mime_type (filter, "image/jpeg"); gtk_file_filter_add_mime_type (filter, "image/png");
  g_autoptr(GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, filter); gtk_file_dialog_set_filters (chooser, G_LIST_MODEL (filters));
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  gtk_file_dialog_open (chooser, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
    self->image_cancel, file_selected, g_object_ref (self));
  (void) button;
}

typedef struct { GdkPixbuf *source; gint x, y, side; } CropJob;
static void crop_job_free (gpointer data) { CropJob *job = data; g_object_unref (job->source); g_free (job); }

static void
encode_crop_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  CropJob *job = data;
  if (g_task_return_error_if_cancelled (task)) return;
  g_autoptr(GdkPixbuf) part = gdk_pixbuf_new_subpixbuf (job->source, job->x, job->y, job->side, job->side);
  g_autoptr(GdkPixbuf) scaled = gdk_pixbuf_scale_simple (part, ART_OUTPUT_PX, ART_OUTPUT_PX, GDK_INTERP_BILINEAR);
  g_autoptr(GdkPixbuf) opaque = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, ART_OUTPUT_PX, ART_OUTPUT_PX);
  gdk_pixbuf_fill (opaque, 0xffffffff);
  gdk_pixbuf_composite (scaled, opaque, 0, 0, ART_OUTPUT_PX, ART_OUTPUT_PX,
    0, 0, 1, 1, GDK_INTERP_BILINEAR, 255);
  for (gint quality = 92; quality >= 32; quality -= 10) {
    gchar *bytes = NULL; gsize length = 0;
    g_autofree gchar *q = g_strdup_printf ("%d", quality);
    g_autoptr(GError) error = NULL;
    if (!gdk_pixbuf_save_to_buffer (opaque, &bytes, &length, "jpeg", &error, "quality", q, NULL)) {
      g_task_return_error (task, g_steal_pointer (&error)); return;
    }
    if (length <= ART_OUTPUT_LIMIT) {
      g_task_return_pointer (task, g_bytes_new_take (bytes, length), (GDestroyNotify) g_bytes_unref); return;
    }
    g_free (bytes);
  }
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The cropped image is too detailed. Try a different crop.");
  (void) source; (void) cancel;
}

static void
crop_encoded (GObject *source, GAsyncResult *result, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) jpeg = g_task_propagate_pointer (G_TASK (result), &error);
  self->busy = FALSE;
  if (!self->closed) {
    if (jpeg) {
      g_clear_pointer (&self->jpeg, g_bytes_unref); self->jpeg = g_bytes_ref (jpeg);
      self->busy = TRUE; update_actions (self);
      gtk_button_set_label (self->next, self->edit_artwork ? "Saving image…" : "Creating…");
      if (self->edit_artwork)
        g_signal_emit (self, signals[ARTWORK_REQUESTED], 0, jpeg);
      else {
        g_autofree gchar *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name))));
        g_signal_emit (self, signals[CREATE_REQUESTED], 0, gtk_check_button_get_active (self->spotify), name);
      }
    } else {
      gtk_label_set_text (self->error, error->message);
      gtk_widget_set_visible (GTK_WIDGET (self->error), TRUE); update_actions (self);
    }
  }
  g_object_unref (self); (void) source;
}

static void
go_next (GtkButton *button, SpotifyGtkPlaylistCreateDialog *self)
{
  (void) button;
  if (self->busy || self->closed ||
      !gtk_widget_get_sensitive (GTK_WIDGET (self->next))) return;
  if (self->step < 2) { set_step (self, self->step + 1); return; }
  if (self->step != 3 || !self->image) return;
  double side, x, y;
  crop_geometry (self, &side, &x, &y);
  CropJob *job = g_new0 (CropJob, 1);
  job->source = g_object_ref (self->image);
  job->side = MAX (1, (gint) floor (side));
  job->x = CLAMP ((gint) floor (x), 0, gdk_pixbuf_get_width (self->image) - job->side);
  job->y = CLAMP ((gint) floor (y), 0, gdk_pixbuf_get_height (self->image) - job->side);
  self->busy = TRUE; update_actions (self);
  GTask *task = g_task_new (NULL, self->image_cancel, crop_encoded, g_object_ref (self));
  g_task_set_task_data (task, job, crop_job_free);
  g_task_run_in_thread (task, encode_crop_worker); g_object_unref (task);
}

static void
skip_image (GtkButton *button, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = data;
  if (self->busy || self->closed || self->edit_artwork) return;
  g_clear_pointer (&self->jpeg, g_bytes_unref);
  g_autofree gchar *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->name))));
  if (!*name || strlen (name) > 256) return;
  self->busy = TRUE; update_actions (self);
  gtk_button_set_label (GTK_BUTTON (self->skip), "Creating…");
  g_signal_emit (self, signals[CREATE_REQUESTED], 0, gtk_check_button_get_active (self->spotify), name);
  (void) button;
}

static void
dialog_closed (AdwDialog *dialog, gpointer data)
{
  SpotifyGtkPlaylistCreateDialog *self = SPOTIFYGTK_PLAYLIST_CREATE_DIALOG (dialog);
  self->closed = TRUE;
  g_cancellable_cancel (self->image_cancel);
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
spotifygtk_playlist_create_dialog_dispose (GObject *object)
{
  SpotifyGtkPlaylistCreateDialog *self = SPOTIFYGTK_PLAYLIST_CREATE_DIALOG (object);
  if (self->image_cancel) g_cancellable_cancel (self->image_cancel);
  g_clear_object (&self->image_cancel);
  g_clear_object (&self->image);
  g_clear_pointer (&self->image_surface, cairo_surface_destroy);
  g_clear_pointer (&self->jpeg, g_bytes_unref);
  G_OBJECT_CLASS (spotifygtk_playlist_create_dialog_parent_class)->dispose (object);
}

static void
spotifygtk_playlist_create_dialog_class_init (SpotifyGtkPlaylistCreateDialogClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = spotifygtk_playlist_create_dialog_dispose;
  signals[ARTWORK_REQUESTED] = g_signal_new ("artwork-requested",
    G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BYTES);
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
  self->image_cancel = g_cancellable_new ();
  adw_dialog_set_content_width (ADW_DIALOG (self), 440);
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
  GtkWidget *choose = gtk_button_new_with_label ("Choose image…");
  self->choose = choose;
  g_signal_connect (choose, "clicked", G_CALLBACK (choose_image), self);
  gtk_stack_add_named (self->steps, choose, "image");
  GtkWidget *crop_step = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  self->crop = gtk_drawing_area_new ();
  gtk_widget_set_size_request (self->crop, 320, 320);
  gtk_widget_set_halign (self->crop, GTK_ALIGN_CENTER);
  gtk_widget_set_cursor_from_name (self->crop, "grab");
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->crop), crop_draw, self, NULL);
  GtkGesture *drag = gtk_gesture_drag_new ();
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag), GTK_PHASE_CAPTURE);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (crop_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (crop_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (crop_drag_end), self);
  g_signal_connect (drag, "cancel", G_CALLBACK (crop_drag_cancel), self);
  gtk_widget_add_controller (self->crop, GTK_EVENT_CONTROLLER (drag));
  gtk_box_append (GTK_BOX (crop_step), self->crop);
  self->zoom = GTK_SCALE (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1, 6, .05));
  gtk_scale_set_draw_value (self->zoom, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->zoom), GTK_ACCESSIBLE_PROPERTY_LABEL, "Image zoom", -1);
  g_signal_connect (self->zoom, "value-changed", G_CALLBACK (crop_changed), self);
  gtk_box_append (GTK_BOX (crop_step), GTK_WIDGET (self->zoom));
  GtkWidget *format = gtk_label_new ("Square JPEG · 640 × 640");
  gtk_widget_add_css_class (format, "caption");
  gtk_widget_add_css_class (format, "dim-label");
  gtk_box_append (GTK_BOX (crop_step), format);
  gtk_stack_add_named (self->steps, crop_step, "crop");
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
  self->skip = gtk_button_new_with_label ("Skip image");
  gtk_box_append (GTK_BOX (actions), self->skip);
  gtk_box_append (GTK_BOX (actions), GTK_WIDGET (self->next));
  g_signal_connect (self->skip, "clicked", G_CALLBACK (skip_image), self);
  gtk_widget_set_margin_top (actions, 8);
  gtk_box_append (GTK_BOX (content), actions);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (self), content);
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
  set_step (self, 0);
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
  gtk_button_set_label (GTK_BUTTON (self->skip), "Skip image");
  gtk_button_set_label (self->next, self->step == 3 ? (self->edit_artwork ? "Save image" : "Create") : "Next");
  update_actions (self);
}

GBytes *
spotifygtk_playlist_create_dialog_dup_artwork (SpotifyGtkPlaylistCreateDialog *self)
{
  g_return_val_if_fail (SPOTIFYGTK_IS_PLAYLIST_CREATE_DIALOG (self), NULL);
  return self->jpeg ? g_bytes_ref (self->jpeg) : NULL;
}

SpotifyGtkPlaylistCreateDialog *
spotifygtk_playlist_artwork_dialog_new (void)
{
  SpotifyGtkPlaylistCreateDialog *self = spotifygtk_playlist_create_dialog_new (FALSE);
  self->edit_artwork = TRUE;
  adw_alert_dialog_set_heading (ADW_ALERT_DIALOG (self), "Change playlist image");
  adw_dialog_set_title (ADW_DIALOG (self), "Change playlist image");
  set_step (self, 2);
  return self;
}
