#include "shortcuts.h"

gboolean
spotifygtk_shortcut_parse (const gchar *text, guint *key, GdkModifierType *mods)
{
  if (!text || !gtk_accelerator_parse (text, key, mods)) return FALSE;
  *key = gdk_keyval_to_lower (*key);
  *mods &= gtk_accelerator_get_default_mod_mask ();
  if ((*mods & ~GDK_SHIFT_MASK) == GDK_CONTROL_MASK &&
      (*key == GDK_KEY_plus || *key == GDK_KEY_equal || *key == GDK_KEY_KP_Add)) {
    *key = GDK_KEY_plus;
    *mods = GDK_CONTROL_MASK;
  }
  if (*mods == GDK_CONTROL_MASK && *key == GDK_KEY_KP_Subtract) *key = GDK_KEY_minus;
  /* Escape and F11 are reserved for dismissal/fullscreen. Unmodified text
   * keys (other than Space) must not steal ordinary typing/navigation. */
  return *key && *key != GDK_KEY_Escape && *key != GDK_KEY_F11 &&
    ((*mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) ||
     (*mods == 0 && (*key == GDK_KEY_space ||
       (*key >= GDK_KEY_F1 && *key <= GDK_KEY_F35))));
}

gboolean
spotifygtk_shortcut_matches (const gchar *text, guint key, GdkModifierType mods)
{
  guint wanted; GdkModifierType modifiers;
  if (!spotifygtk_shortcut_parse (text, &wanted, &modifiers)) return FALSE;
  mods &= gtk_accelerator_get_default_mod_mask ();
  /* Ctrl++ is Shift+= on many keyboards; accept Ctrl+= and keypad +/- too.
   * Shift needed to produce a plus is not an additional binding modifier. */
  if (wanted == GDK_KEY_plus && modifiers == GDK_CONTROL_MASK &&
      (key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_KP_Add))
    return (mods & ~GDK_SHIFT_MASK) == GDK_CONTROL_MASK;
  if (wanted == GDK_KEY_minus && modifiers == GDK_CONTROL_MASK && key == GDK_KEY_KP_Subtract)
    key = GDK_KEY_minus;
  return gdk_keyval_to_lower (key) == wanted && mods == modifiers;
}

gboolean
spotifygtk_shortcut_is_typing (GtkWidget *focus)
{
  for (GtkWidget *widget = focus; widget; widget = gtk_widget_get_parent (widget))
    if (GTK_IS_EDITABLE (widget) || GTK_IS_TEXT_VIEW (widget)) return TRUE;
  return FALSE;
}

static void
update_button (GtkButton *button)
{
  SpotifyGtkShortcut action = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "shortcut-action"));
  guint key = 0; GdkModifierType mods = 0;
  spotifygtk_shortcut_parse (spotifygtk_settings_get_shortcut (spotifygtk_settings_get_default (), action), &key, &mods);
  g_autofree gchar *label = key ? gtk_accelerator_get_label (key, mods) : g_strdup ("Set shortcut");
  gtk_button_set_label (button, label);
}

static void
cancel_recording (GtkButton *button)
{
  g_autoptr(GtkButton) keep_alive = g_object_ref (button);
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));
  if (root && g_object_get_data (G_OBJECT (root), "shortcut-recording") == button)
    g_object_set_data (G_OBJECT (root), "shortcut-recording", NULL);
  g_object_set_data (G_OBJECT (button), "recording", NULL);
  update_button (button);
}

static void
record_clicked (GtkButton *button, gpointer unused)
{
  (void) unused;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));
  if (!root) return;
  GtkButton *previous = g_object_get_data (G_OBJECT (root), "shortcut-recording");
  if (previous) cancel_recording (previous);
  g_object_set_data_full (G_OBJECT (root), "shortcut-recording", g_object_ref (button), g_object_unref);
  g_object_set_data (G_OBJECT (button), "recording", GINT_TO_POINTER (1));
  gtk_widget_grab_focus (GTK_WIDGET (button));
  gtk_button_set_label (button, "Press keys… (Esc cancels)");
}

static gboolean
record_key (GtkEventControllerKey *controller, guint key, guint code,
            GdkModifierType mods, GtkButton *button)
{
  (void) controller; (void) code;
  if (!g_object_get_data (G_OBJECT (button), "recording")) return GDK_EVENT_PROPAGATE;
  if (key == GDK_KEY_Escape) { cancel_recording (button); return GDK_EVENT_STOP; }
  if (key == GDK_KEY_Control_L || key == GDK_KEY_Control_R || key == GDK_KEY_Shift_L ||
      key == GDK_KEY_Shift_R || key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R ||
      key == GDK_KEY_Super_L || key == GDK_KEY_Super_R) return GDK_EVENT_STOP;
  mods &= gtk_accelerator_get_default_mod_mask ();
  if (key == GDK_KEY_plus && (mods & GDK_CONTROL_MASK)) mods &= ~GDK_SHIFT_MASK;
  g_autofree gchar *name = gtk_accelerator_name (key, mods);
  guint parsed; GdkModifierType parsed_mods;
  if (!spotifygtk_shortcut_parse (name, &parsed, &parsed_mods)) {
    gtk_button_set_label (button, "Use Ctrl/Alt + key, Space or F-key");
    return GDK_EVENT_STOP;
  }
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  guint action = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "shortcut-action"));
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_COUNT; i++)
    if (i != action && spotifygtk_shortcut_matches (spotifygtk_settings_get_shortcut (settings, i), key, mods)) {
      gtk_button_set_label (button, "Already assigned — choose another");
      return GDK_EVENT_STOP;
    }
  spotifygtk_settings_set_shortcut (settings, action, name);
  cancel_recording (button);
  return GDK_EVENT_STOP;
}

static void
record_focus_left (GtkEventControllerFocus *controller, GtkButton *button)
{
  (void) controller;
  cancel_recording (button);
}

static void
reset_clicked (GtkButton *reset, GtkWidget *box)
{
  (void) reset;
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_COUNT; i++)
    spotifygtk_settings_set_shortcut (settings, i, NULL);
  for (GtkWidget *row = gtk_widget_get_first_child (box); row; row = gtk_widget_get_next_sibling (row)) {
    GtkWidget *button = g_object_get_data (G_OBJECT (row), "shortcut-button");
    if (button) cancel_recording (GTK_BUTTON (button));
  }
}

GtkWidget *
spotifygtk_shortcuts_editor_new (void)
{
  static const gchar *names[] = { "Increase scale", "Decrease scale", "Play / pause" };
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_COUNT; i++) {
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *label = gtk_label_new (names[i]);
    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_widget_set_hexpand (label, TRUE);
    gtk_box_append (GTK_BOX (row), label);
    GtkWidget *button = gtk_button_new ();
    gtk_widget_add_css_class (button, "settings-action");
    g_object_set_data (G_OBJECT (button), "shortcut-action", GUINT_TO_POINTER (i));
    g_object_set_data (G_OBJECT (row), "shortcut-button", button);
    update_button (GTK_BUTTON (button));
    gtk_widget_set_tooltip_text (button, "Click, then press a shortcut. Escape cancels.");
    g_signal_connect (button, "clicked", G_CALLBACK (record_clicked), NULL);
    GtkEventController *keys = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (record_key), button);
    gtk_widget_add_controller (button, keys);
    GtkEventController *focus = gtk_event_controller_focus_new ();
    g_signal_connect (focus, "leave", G_CALLBACK (record_focus_left), button);
    gtk_widget_add_controller (button, focus);
    gtk_box_append (GTK_BOX (row), button);
    gtk_box_append (GTK_BOX (box), row);
  }
  GtkWidget *reset = gtk_button_new_with_label ("Restore default shortcuts");
  gtk_widget_set_halign (reset, GTK_ALIGN_END);
  gtk_widget_add_css_class (reset, "settings-action");
  g_signal_connect (reset, "clicked", G_CALLBACK (reset_clicked), box);
  gtk_box_append (GTK_BOX (box), reset);
  return box;
}
