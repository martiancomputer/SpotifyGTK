#include "../src/ui/playlist_create_dialog.c"
#include "../src/ui/dialog_host.c"

typedef struct {
  guint count;
  gboolean spotify;
  gchar *name;
} Creation;

static void
requested (SpotifyGtkPlaylistCreateDialog *dialog, gboolean spotify,
           const gchar *name, Creation *creation)
{
  (void) dialog;
  creation->count++;
  creation->spotify = spotify;
  g_free (creation->name);
  creation->name = g_strdup (name);
}

static void
test_steps_and_retry (void)
{
  g_autoptr(SpotifyGtkPlaylistCreateDialog) dialog = g_object_ref_sink (
    spotifygtk_playlist_create_dialog_new (TRUE));
  Creation creation = {0};
  g_signal_connect (dialog, "create-requested", G_CALLBACK (requested), &creation);
  g_assert_true (gtk_check_button_get_active (dialog->spotify));
  go_next (dialog->next, dialog);
  g_assert_true (dialog->naming);
  g_assert_false (gtk_widget_get_sensitive (GTK_WIDGET (dialog->next)));
  gtk_editable_set_text (GTK_EDITABLE (dialog->name), "   ");
  go_next (dialog->next, dialog);
  g_assert_cmpuint (creation.count, ==, 0);
  gtk_editable_set_text (GTK_EDITABLE (dialog->name), "  New playlist  ");
  go_back (dialog->back, dialog);
  g_assert_false (dialog->naming);
  go_next (dialog->next, dialog);
  g_assert_cmpstr (gtk_editable_get_text (GTK_EDITABLE (dialog->name)), ==, "  New playlist  ");
  go_next (dialog->next, dialog);
  go_next (dialog->next, dialog);
  g_assert_cmpuint (creation.count, ==, 1);
  g_assert_true (creation.spotify);
  g_assert_cmpstr (creation.name, ==, "New playlist");
  g_assert_true (dialog->busy);
  go_back (dialog->back, dialog);
  g_assert_true (dialog->naming);
  spotifygtk_playlist_create_dialog_complete (dialog, "Try again");
  g_assert_false (dialog->busy);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (dialog->error)));
  g_assert_true (gtk_widget_get_sensitive (GTK_WIDGET (dialog->next)));
  go_next (dialog->next, dialog);
  g_assert_cmpuint (creation.count, ==, 2);
  g_free (creation.name);
}

static void
test_signed_out_and_byte_limit (void)
{
  g_autoptr(SpotifyGtkPlaylistCreateDialog) dialog = g_object_ref_sink (
    spotifygtk_playlist_create_dialog_new (FALSE));
  g_assert_false (gtk_widget_get_sensitive (GTK_WIDGET (dialog->spotify)));
  g_assert_false (gtk_check_button_get_active (dialog->spotify));
  Creation creation = {0};
  g_signal_connect (dialog, "create-requested", G_CALLBACK (requested), &creation);
  go_next (dialog->next, dialog);
  GString *name = g_string_new (NULL);
  for (int i = 0; i < 100; i++) g_string_append (name, "曲");
  gtk_editable_set_text (GTK_EDITABLE (dialog->name), name->str);
  g_string_free (name, TRUE);
  g_assert_false (gtk_widget_get_sensitive (GTK_WIDGET (dialog->next)));
  go_next (dialog->next, dialog);
  g_assert_cmpuint (creation.count, ==, 0);
  gtk_editable_set_text (GTK_EDITABLE (dialog->name), "Device playlist");
  go_next (dialog->next, dialog);
  g_assert_cmpuint (creation.count, ==, 1);
  g_assert_false (creation.spotify);
  g_free (creation.name);
  /* A late completion cannot reopen a dismissed dialog. */
  dialog_closed (ADW_DIALOG (dialog), NULL);
  gtk_widget_set_visible (GTK_WIDGET (dialog->error), FALSE);
  spotifygtk_playlist_create_dialog_complete (dialog, "Late failure");
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (dialog->error)));
}

static void
settle (void)
{
  gint64 deadline = g_get_monotonic_time () + 250000;
  while (g_get_monotonic_time () < deadline) {
    g_main_context_iteration (NULL, FALSE);
    g_usleep (1000);
  }
}

static void
test_present_keyboard_and_close (void)
{
  GtkWidget *parent = g_object_new (ADW_TYPE_APPLICATION_WINDOW, NULL);
  spotifygtk_dialog_host_bind (ADW_APPLICATION_WINDOW (parent));
  gtk_window_set_default_size (GTK_WINDOW (parent), 800, 600);
  gtk_window_present (GTK_WINDOW (parent));
  g_autoptr(SpotifyGtkPlaylistCreateDialog) dialog = g_object_ref_sink (
    spotifygtk_playlist_create_dialog_new (TRUE));
  Creation creation = {0};
  g_signal_connect (dialog, "create-requested", G_CALLBACK (requested), &creation);
  adw_dialog_present (ADW_DIALOG (dialog), parent);
  settle ();
  g_assert_true (gtk_widget_get_mapped (GTK_WIDGET (dialog)));
  g_assert_true (gtk_widget_get_root (GTK_WIDGET (dialog)) == GTK_ROOT (parent));
  go_next (dialog->next, dialog);
  settle ();
  g_assert_false (dialog->closed);
  GtkWidget *focus = adw_dialog_get_focus (ADW_DIALOG (dialog));
  g_assert_nonnull (focus);
  g_assert_true (focus == GTK_WIDGET (dialog->name) ||
    gtk_widget_is_ancestor (focus, GTK_WIDGET (dialog->name)));
  gtk_editable_set_text (GTK_EDITABLE (dialog->name), "Keyboard playlist");
  g_signal_emit_by_name (dialog->name, "activate");
  settle ();
  g_assert_cmpuint (creation.count, ==, 1);
  g_assert_false (dialog->closed);
  spotifygtk_playlist_create_dialog_complete (dialog, NULL);
  settle ();
  g_assert_true (dialog->closed);
  gtk_window_destroy (GTK_WINDOW (parent));
  g_free (creation.name);
}

static void
background_clicked (GtkButton *button, guint *count)
{
  (void) button;
  (*count)++;
}

static void
test_embedded_dismissal (void)
{
  GtkWidget *parent = g_object_new (ADW_TYPE_APPLICATION_WINDOW, NULL);
  spotifygtk_dialog_host_bind (ADW_APPLICATION_WINDOW (parent));
  GtkWidget *background = gtk_button_new_with_label ("Background action");
  guint background_actions = 0;
  g_signal_connect (background, "clicked", G_CALLBACK (background_clicked), &background_actions);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (parent), background);
  gtk_window_set_default_size (GTK_WINDOW (parent), 800, 600);
  gtk_window_present (GTK_WINDOW (parent));
  for (guint escape = 0; escape < 2; escape++) {
    g_autoptr(SpotifyGtkPlaylistCreateDialog) dialog = g_object_ref_sink (
      spotifygtk_playlist_create_dialog_new (TRUE));
    spotifygtk_dialog_prepare (ADW_DIALOG (dialog));
    Creation creation = {0};
    g_signal_connect (dialog, "create-requested", G_CALLBACK (requested), &creation);
    adw_dialog_present (ADW_DIALOG (dialog), parent);
    settle ();
    go_next (dialog->next, dialog);
    gtk_editable_set_text (GTK_EDITABLE (dialog->name), "Unconfirmed playlist");
    settle ();
    g_assert_true (gtk_widget_get_root (GTK_WIDGET (dialog)) == GTK_ROOT (parent));
    g_assert_cmpint (adw_dialog_get_presentation_mode (ADW_DIALOG (dialog)), ==, ADW_DIALOG_FLOATING);
    g_assert_true (adw_dialog_get_can_close (ADW_DIALOG (dialog)));
    graphene_rect_t bounds;
    g_assert_true (gtk_widget_compute_bounds (adw_dialog_get_child (ADW_DIALOG (dialog)), parent, &bounds));
    g_assert_cmpfloat (bounds.size.width, <, gtk_widget_get_width (parent));
    g_autoptr(GListModel) controllers = gtk_widget_observe_controllers (parent);
    gboolean handled = FALSE;
    for (guint i = 0; i < g_list_model_get_n_items (controllers); i++) {
      g_autoptr(GObject) controller = g_list_model_get_item (controllers, i);
      if (gtk_event_controller_get_propagation_phase (GTK_EVENT_CONTROLLER (controller)) != GTK_PHASE_CAPTURE)
        continue;
      if (!escape && GTK_IS_GESTURE_CLICK (controller)) {
        /* A click inside must not dismiss; the same controller catches the
         * dimmed area outside before it can reach background controls. */
        g_signal_emit_by_name (controller, "pressed", 1,
          bounds.origin.x + bounds.size.width / 2,
          bounds.origin.y + bounds.size.height / 2);
        g_assert_false (dialog->closed);
        g_signal_emit_by_name (controller, "pressed", 1, 50.0, 50.0);
        handled = TRUE;
        break;
      }
      if (escape && GTK_IS_EVENT_CONTROLLER_KEY (controller)) {
        g_signal_emit_by_name (controller, "key-pressed", GDK_KEY_Escape,
                               0, (GdkModifierType) 0, &handled);
        if (handled) break;
      }
    }
    settle ();
    g_assert_true (handled);
    g_assert_true (dialog->closed);
    g_assert_cmpuint (creation.count, ==, 0);
    g_assert_cmpuint (background_actions, ==, 0);
    g_assert_null (adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (parent)));
    g_free (creation.name);
  }
  gtk_window_destroy (GTK_WINDOW (parent));
}

typedef struct { guint count; gchar *response; } Cancellation;

static void
alert_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  Cancellation *cancel = data;
  cancel->count++;
  cancel->response = g_strdup (adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), result));
}

static void
test_alert_cancel_response (void)
{
  GtkWidget *parent = g_object_new (ADW_TYPE_APPLICATION_WINDOW, NULL);
  spotifygtk_dialog_host_bind (ADW_APPLICATION_WINDOW (parent));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (parent), gtk_label_new ("Main window"));
  gtk_window_set_default_size (GTK_WINDOW (parent), 800, 600);
  gtk_window_present (GTK_WINDOW (parent));
  for (guint escape = 0; escape < 2; escape++) {
    Cancellation cancel = {0};
    g_autoptr(AdwAlertDialog) alert = g_object_ref_sink (ADW_ALERT_DIALOG (
      adw_alert_dialog_new ("Confirm action", "Dismissal must cancel, not confirm.")));
    spotifygtk_dialog_prepare (ADW_DIALOG (alert));
    adw_alert_dialog_add_responses (alert, "cancel", "Cancel", "confirm", "Confirm", NULL);
    adw_alert_dialog_set_default_response (alert, "confirm");
    adw_alert_dialog_set_close_response (alert, "cancel");
    adw_alert_dialog_choose (alert, parent, NULL, alert_chosen, &cancel);
    settle ();
    g_assert_true (gtk_widget_get_root (GTK_WIDGET (alert)) == GTK_ROOT (parent));
    g_autoptr(GListModel) controllers = gtk_widget_observe_controllers (parent);
    gboolean handled = FALSE;
    for (guint i = 0; i < g_list_model_get_n_items (controllers); i++) {
      g_autoptr(GObject) controller = g_list_model_get_item (controllers, i);
      if (gtk_event_controller_get_propagation_phase (GTK_EVENT_CONTROLLER (controller)) != GTK_PHASE_CAPTURE)
        continue;
      if (!escape && GTK_IS_GESTURE_CLICK (controller)) {
        g_signal_emit_by_name (controller, "pressed", 1, 50.0, 50.0);
        handled = TRUE;
        break;
      }
      if (escape && GTK_IS_EVENT_CONTROLLER_KEY (controller)) {
        g_signal_emit_by_name (controller, "key-pressed", GDK_KEY_Escape,
          0, (GdkModifierType) 0, &handled);
        if (handled) break;
      }
    }
    settle ();
    g_assert_true (handled);
    g_assert_cmpuint (cancel.count, ==, 1);
    g_assert_cmpstr (cancel.response, ==, "cancel");
    g_free (cancel.response);
  }
  gtk_window_destroy (GTK_WINDOW (parent));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ()) return 77;
  /* Desktop GTK preferences must not make an unrelated Adwaita warning
   * fatal in this isolated test process. This does not change user settings. */
#if GTK_CHECK_VERSION(4, 24, 0)
  g_object_set (gtk_settings_get_default (), "gtk-interface-color-scheme",
                 GTK_INTERFACE_COLOR_SCHEME_DEFAULT, NULL);
#else
  g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", FALSE, NULL);
#endif
  adw_init ();
  g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
  g_test_add_func ("/playlist-create/steps-retry", test_steps_and_retry);
  g_test_add_func ("/playlist-create/signed-out-byte-limit", test_signed_out_and_byte_limit);
  g_test_add_func ("/playlist-create/present-keyboard-close", test_present_keyboard_and_close);
  g_test_add_func ("/playlist-create/embedded-dismissal", test_embedded_dismissal);
  g_test_add_func ("/playlist-create/alert-cancel-response", test_alert_cancel_response);
  return g_test_run ();
}
