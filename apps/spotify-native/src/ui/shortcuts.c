#include "shortcuts.h"
#include "dialog_host.h"
#include <string.h>

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
  /* Escape is Cancel; F11 is intentionally unassigned. Unmodified text
   * keys (other than Space) must not steal ordinary typing/navigation. */
  return *key && *key != GDK_KEY_Escape && *key != GDK_KEY_F11 &&
    ((*mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) ||
     (*mods == GDK_SHIFT_MASK && (*key == GDK_KEY_Left || *key == GDK_KEY_Right)) ||
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

gboolean
spotifygtk_shortcut_allowed_while_typing (SpotifyGtkShortcut action,
                                         guint key, GdkModifierType mods)
{
  const SpotifyGtkShortcutInfo *info = spotifygtk_shortcut_info (action);
  if (!info || !info->allow_typing) return FALSE;
  key = gdk_keyval_to_lower (key);
  mods &= gtk_accelerator_get_default_mod_mask ();
  /* Remapping Home to Ctrl+A must not steal Select All, nor may remapping
   * Search to Space steal spaces. Only non-editing combinations pass. */
  if (mods == GDK_CONTROL_MASK &&
      ((key >= GDK_KEY_1 && key <= GDK_KEY_5) || key == GDK_KEY_f ||
       key == GDK_KEY_minus || key == GDK_KEY_KP_Subtract)) return TRUE;
  return (mods & ~GDK_SHIFT_MASK) == GDK_CONTROL_MASK &&
    (key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_KP_Add);
}

GStrv
spotifygtk_shortcut_keycap_labels (const gchar *text)
{
  guint key = 0; GdkModifierType mods = 0;
  GPtrArray *labels = g_ptr_array_new ();
  if (spotifygtk_shortcut_parse (text, &key, &mods)) {
    if (mods & GDK_CONTROL_MASK) g_ptr_array_add (labels, g_strdup ("Ctrl"));
    if (mods & GDK_ALT_MASK) g_ptr_array_add (labels, g_strdup ("Alt"));
    if (mods & GDK_SHIFT_MASK) g_ptr_array_add (labels, g_strdup ("Shift"));
    if (mods & GDK_SUPER_MASK) g_ptr_array_add (labels, g_strdup ("Super"));
    if (mods & GDK_META_MASK) g_ptr_array_add (labels, g_strdup ("Meta"));
    if (mods & GDK_HYPER_MASK) g_ptr_array_add (labels, g_strdup ("Hyper"));
    g_ptr_array_add (labels, key == GDK_KEY_space ? g_strdup ("Space") :
      gtk_accelerator_get_label (gdk_keyval_to_upper (key), 0));
  }
  g_ptr_array_add (labels, NULL);
  return (GStrv) g_ptr_array_free (labels, FALSE);
}

gint
spotifygtk_shortcut_find_conflict (SpotifyGtkSettings *settings,
                                  SpotifyGtkShortcut action, const gchar *accelerator)
{
  guint key; GdkModifierType mods;
  if (!spotifygtk_shortcut_parse (accelerator, &key, &mods)) return -1;
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_COUNT; i++) {
    if (i != (guint) action && spotifygtk_shortcut_matches (
          spotifygtk_settings_get_shortcut (settings, i), key, mods)) return (gint) i;
  }
  return -1;
}

gboolean
spotifygtk_shortcut_search_matches (const gchar *query, const gchar *label,
                                    const gchar *description, const gchar *accelerator)
{
  if (!query || !*query) return TRUE;
  guint key = 0;
  GdkModifierType mods = 0;
  g_autofree gchar *display = NULL;
  if (accelerator && gtk_accelerator_parse (accelerator, &key, &mods))
    display = gtk_accelerator_get_label (key, mods);
  g_autofree gchar *text = g_strjoin (" ", label ? label : "",
    description ? description : "", accelerator ? accelerator : "",
    display ? display : "", NULL);
  g_autofree gchar *folded = g_utf8_casefold (text, -1);
  g_autofree gchar *folded_query = g_utf8_casefold (query, -1);
  g_auto(GStrv) words = g_strsplit_set (folded_query, " \t\r\n", -1);
  for (guint i = 0; words[i]; i++)
    if (*words[i] && !strstr (folded, words[i])) return FALSE;
  return TRUE;
}

static void
set_recording_hint (GtkButton *button, const gchar *text)
{
  GtkWidget *row_status = g_object_get_data (G_OBJECT (button), "shortcut-row-status");
  if (row_status) {
    gboolean recording = g_object_get_data (G_OBJECT (button), "recording") != NULL;
    gtk_widget_set_visible (row_status, recording);
    if (recording) {
      g_autofree gchar *message = g_strdup_printf ("%s · Esc to cancel", text);
      gtk_label_set_text (GTK_LABEL (row_status), message);
    }
  }
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, text, -1);
}

static void
update_button (GtkButton *button)
{
  SpotifyGtkShortcut action = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "shortcut-action"));
  const gchar *binding = spotifygtk_settings_get_shortcut (spotifygtk_settings_get_default (), action);
  if (g_strcmp0 (binding, g_object_get_data (G_OBJECT (button), "shortcut-rendered-binding")) == 0) return;
  g_object_set_data_full (G_OBJECT (button), "shortcut-rendered-binding", g_strdup (binding), g_free);
  g_auto(GStrv) legends = spotifygtk_shortcut_keycap_labels (binding);
  GtkWidget *keys = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (keys, GTK_ALIGN_END);
  for (guint i = 0; legends[i]; i++) {
    if (i) {
      GtkWidget *join = gtk_label_new ("+");
      gtk_widget_add_css_class (join, "keycap-join");
      gtk_box_append (GTK_BOX (keys), join);
    }
    GtkWidget *key = gtk_label_new (legends[i]);
    gtk_widget_add_css_class (key, "keycap");
    if (g_str_equal (legends[i], "Space")) gtk_widget_add_css_class (key, "keycap-wide");
    else if (g_utf8_strlen (legends[i], -1) > 1) gtk_widget_add_css_class (key, "keycap-modifier");
    else gtk_widget_add_css_class (key, "keycap-glyph");
    gtk_box_append (GTK_BOX (keys), key);
  }
  if (!legends[0]) gtk_box_append (GTK_BOX (keys), gtk_label_new ("Set shortcut"));
  gtk_button_set_child (button, keys);
  guint key = 0; GdkModifierType mods = 0;
  spotifygtk_shortcut_parse (binding, &key, &mods);
  g_autofree gchar *display = key ? gtk_accelerator_get_label (key, mods) : g_strdup ("Not assigned");
  g_autofree gchar *accessible = g_strdup_printf ("%s: %s. Activate to record a shortcut.",
    (const gchar *) g_object_get_data (G_OBJECT (button), "shortcut-name"), display);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
  /* Keep an open search accurate when a shortcut is changed or reset. */
  GtkWidget *row = gtk_widget_get_ancestor (GTK_WIDGET (button), GTK_TYPE_LIST_BOX_ROW);
  if (row) gtk_list_box_row_changed (GTK_LIST_BOX_ROW (row));
}

static void
recording_root_free (gpointer data)
{
  g_weak_ref_clear (data);
  g_free (data);
}

static void
cancel_recording (GtkButton *button)
{
  g_autoptr(GtkButton) keep_alive = g_object_ref (button);
  GWeakRef *recording_root = g_object_get_data (G_OBJECT (button), "shortcut-recording-root");
  g_autoptr(GObject) root = recording_root ? g_weak_ref_get (recording_root) : NULL;
  if (root && g_object_get_data (G_OBJECT (root), "shortcut-recording") == button)
    g_object_set_data (G_OBJECT (root), "shortcut-recording", NULL);
  g_object_set_data (G_OBJECT (button), "shortcut-recording-root", NULL);
  g_object_set_data (G_OBJECT (button), "recording", NULL);
  gtk_widget_remove_css_class (GTK_WIDGET (button), "recording");
  set_recording_hint (button, "Click to change shortcut");
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
  GWeakRef *recording_root = g_new0 (GWeakRef, 1);
  g_weak_ref_init (recording_root, root);
  g_object_set_data_full (G_OBJECT (button), "shortcut-recording-root",
                          recording_root, recording_root_free);
  g_object_set_data_full (G_OBJECT (root), "shortcut-recording", g_object_ref (button), g_object_unref);
  g_object_set_data (G_OBJECT (button), "recording", GINT_TO_POINTER (1));
  gtk_widget_add_css_class (GTK_WIDGET (button), "recording");
  gtk_widget_grab_focus (GTK_WIDGET (button));
  set_recording_hint (button, "Press keys…");
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
    set_recording_hint (button, "Use Ctrl / Alt + a key, Space or F-key");
    return GDK_EVENT_STOP;
  }
  SpotifyGtkSettings *settings = spotifygtk_settings_get_default ();
  guint action = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "shortcut-action"));
  gint conflict = spotifygtk_shortcut_find_conflict (settings, action, name);
  if (conflict >= 0) {
    g_autofree gchar *message = g_strdup_printf ("Already assigned to %s.", spotifygtk_shortcut_info (conflict)->label);
    set_recording_hint (button, message);
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
  if (g_object_get_data (G_OBJECT (button), "recording")) cancel_recording (button);
}

static void
record_unmapped (GtkWidget *button, gpointer unused)
{
  (void) unused;
  if (g_object_get_data (G_OBJECT (button), "recording"))
    cancel_recording (GTK_BUTTON (button));
}

static void
shortcut_setting_changed (SpotifyGtkSettings *settings, GtkButton *button)
{
  if (!g_object_get_data (G_OBJECT (button), "recording")) update_button (button);
  (void) settings;
}

void
spotifygtk_shortcuts_restore_defaults (SpotifyGtkSettings *settings)
{
  g_return_if_fail (SPOTIFYGTK_IS_SETTINGS (settings));
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_COUNT; i++)
    spotifygtk_settings_set_shortcut (settings, i, NULL);
}

static void
reset_clicked (GtkButton *reset, GtkWidget *box)
{
  (void) reset;
  GtkRoot *root = gtk_widget_get_root (box);
  GtkButton *recording = root ? g_object_get_data (G_OBJECT (root), "shortcut-recording") : NULL;
  if (recording) cancel_recording (recording);
  spotifygtk_shortcuts_restore_defaults (spotifygtk_settings_get_default ());
}

static gboolean
filter_shortcut_row (GtkListBoxRow *row, gpointer unused)
{
  (void) unused;
  GtkWidget *list = gtk_widget_get_parent (GTK_WIDGET (row));
  const gchar *query = list ? g_object_get_data (G_OBJECT (list), "shortcut-query") : NULL;
  GtkButton *button = g_object_get_data (G_OBJECT (row), "shortcut-button");
  const gchar *binding = button
    ? g_object_get_data (G_OBJECT (button), "shortcut-rendered-binding") : "Escape";
  return spotifygtk_shortcut_search_matches (query,
    g_object_get_data (G_OBJECT (row), "shortcut-name"),
    g_object_get_data (G_OBJECT (row), "shortcut-description"), binding);
}

static void
search_changed (GtkEditable *search, GtkListBox *list)
{
  g_object_set_data_full (G_OBJECT (list), "shortcut-query",
    g_strdup (gtk_editable_get_text (search)), g_free);
  gtk_list_box_invalidate_filter (list);
  GtkWidget *scroll = gtk_widget_get_ancestor (GTK_WIDGET (list), GTK_TYPE_SCROLLED_WINDOW);
  if (scroll) gtk_adjustment_set_value (
    gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroll)), 0);
}

static GtkWidget *
shortcuts_editor_new (void)
{
  GtkWidget *list = gtk_list_box_new ();
  gtk_widget_add_css_class (list, "shortcuts-editor");
  gtk_widget_set_margin_end (list, 8);
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_list_box_set_show_separators (GTK_LIST_BOX (list), TRUE);
  gtk_list_box_set_filter_func (GTK_LIST_BOX (list), filter_shortcut_row, NULL, NULL);
  GtkWidget *empty = gtk_label_new ("No shortcuts found");
  gtk_widget_add_css_class (empty, "dim-text");
  gtk_widget_set_margin_top (empty, 24);
  gtk_widget_set_margin_bottom (empty, 24);
  gtk_list_box_set_placeholder (GTK_LIST_BOX (list), empty);
  for (guint i = 0; i < SPOTIFYGTK_SHORTCUT_COUNT; i++) {
    const SpotifyGtkShortcutInfo *info = spotifygtk_shortcut_info (i);
    GtkWidget *list_row = gtk_list_box_row_new ();
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (list_row), FALSE);
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (list_row), FALSE);
    g_object_set_data (G_OBJECT (list_row), "shortcut-name", (gpointer) info->label);
    g_object_set_data (G_OBJECT (list_row), "shortcut-description", (gpointer) info->description);
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (row, "shortcut-row");
    GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand (text, TRUE);
    GtkWidget *label = gtk_label_new (info->label);
    gtk_label_set_xalign (GTK_LABEL (label), 0);
    gtk_label_set_wrap (GTK_LABEL (label), TRUE);
    gtk_widget_add_css_class (label, "shortcut-name");
    gtk_box_append (GTK_BOX (text), label);
    gtk_widget_set_tooltip_text (label, info->description);
    GtkWidget *row_status = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (row_status), 0);
    gtk_label_set_wrap (GTK_LABEL (row_status), TRUE);
    gtk_widget_add_css_class (row_status, "shortcut-record-status");
    gtk_widget_set_visible (row_status, FALSE);
    gtk_box_append (GTK_BOX (text), row_status);
    gtk_box_append (GTK_BOX (row), text);
    GtkWidget *button = gtk_button_new ();
    gtk_widget_add_css_class (button, "shortcut-binding");
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    g_object_set_data (G_OBJECT (button), "shortcut-action", GUINT_TO_POINTER (i));
    g_object_set_data (G_OBJECT (button), "shortcut-name", (gpointer) info->label);
    g_object_set_data (G_OBJECT (button), "shortcut-row-status", row_status);
    g_object_set_data (G_OBJECT (list_row), "shortcut-button", button);
    update_button (GTK_BUTTON (button));
    gtk_widget_set_tooltip_text (button, "Click, then press a shortcut. Escape cancels.");
    g_signal_connect (button, "clicked", G_CALLBACK (record_clicked), NULL);
    g_signal_connect (button, "unmap", G_CALLBACK (record_unmapped), NULL);
    g_signal_connect_object (spotifygtk_settings_get_default (), "changed",
                             G_CALLBACK (shortcut_setting_changed), button, 0);
    GtkEventController *keys = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (record_key), button);
    gtk_widget_add_controller (button, keys);
    GtkEventController *focus = gtk_event_controller_focus_new ();
    g_signal_connect (focus, "leave", G_CALLBACK (record_focus_left), button);
    gtk_widget_add_controller (button, focus);
    gtk_box_append (GTK_BOX (row), button);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (list_row), row);
    gtk_list_box_append (GTK_LIST_BOX (list), list_row);
  }
  /* Escape is visible and searchable, but remains reserved for cancellation. */
  {
    GtkWidget *list_row = gtk_list_box_row_new ();
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (list_row), FALSE);
    gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (list_row), FALSE);
    g_object_set_data (G_OBJECT (list_row), "shortcut-name", "Cancel");
    g_object_set_data (G_OBJECT (list_row), "shortcut-description", "Reserved, not rebindable");
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (row, "shortcut-row");
    GtkWidget *text = gtk_label_new ("Cancel · Reserved, not rebindable");
    gtk_label_set_xalign (GTK_LABEL (text), 0);
    gtk_label_set_wrap (GTK_LABEL (text), TRUE);
    gtk_widget_set_hexpand (text, TRUE);
    gtk_widget_add_css_class (text, "dim-text");
    gtk_box_append (GTK_BOX (row), text);
    GtkWidget *escape = gtk_label_new ("Esc");
    gtk_widget_add_css_class (escape, "keycap");
    gtk_widget_add_css_class (escape, "keycap-modifier");
    gtk_box_append (GTK_BOX (row), escape);
    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (list_row), row);
    gtk_list_box_append (GTK_LIST_BOX (list), list_row);
  }
  return list;
}

static void
edit_clicked (GtkButton *button, gpointer unused)
{
  (void) unused;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));
  if (!ADW_IS_APPLICATION_WINDOW (root) ||
      adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (root)))
    return;

  /* Build on demand. The presentation owns the dialog; no Settings-page
   * reference retains its keycaps or settings signal handlers after closing. */
  AdwDialog *dialog = adw_dialog_new ();
  spotifygtk_dialog_prepare (dialog);
  adw_dialog_set_title (dialog, "Keyboard shortcuts");
  adw_dialog_set_content_width (dialog, 620);
  adw_dialog_set_content_height (dialog, 560);
  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (body, 18);
  gtk_widget_set_margin_bottom (body, 18);
  gtk_widget_set_margin_start (body, 20);
  gtk_widget_set_margin_end (body, 20);
  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *title = gtk_label_new ("Keyboard shortcuts");
  gtk_widget_add_css_class (title, "dialog-title");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_box_append (GTK_BOX (header), title);
  GtkWidget *close = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (close, "flat");
  gtk_widget_set_tooltip_text (close, "Close");
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (adw_dialog_close), dialog);
  gtk_box_append (GTK_BOX (header), close);
  gtk_box_append (GTK_BOX (body), header);
  GtkWidget *search = gtk_search_entry_new ();
  g_object_set (search, "placeholder-text", "Search shortcuts", NULL);
  gtk_widget_set_tooltip_text (search,
    "Search actions or key combinations. Click a keybind to edit; Esc cancels recording.");
  gtk_accessible_update_property (GTK_ACCESSIBLE (search),
    GTK_ACCESSIBLE_PROPERTY_LABEL, "Search keyboard shortcuts", -1);
  gtk_box_append (GTK_BOX (body), search);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroll), FALSE);
  GtkWidget *list = shortcuts_editor_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list);
  g_signal_connect_object (search, "changed", G_CALLBACK (search_changed), list, 0);
  gtk_box_append (GTK_BOX (body), scroll);
  adw_dialog_set_child (dialog, body);
  adw_dialog_set_focus (dialog, search);
  adw_dialog_present (dialog, GTK_WIDGET (button));
}

GtkWidget *
spotifygtk_shortcuts_manager_new (void)
{
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *reset = gtk_button_new_with_label ("Restore Defaults");
  gtk_widget_add_css_class (reset, "settings-action");
  gtk_widget_set_tooltip_text (reset, "Reset all keyboard shortcuts");
  g_signal_connect (reset, "clicked", G_CALLBACK (reset_clicked), actions);
  gtk_box_append (GTK_BOX (actions), reset);
  GtkWidget *edit = gtk_button_new_with_label ("Edit");
  gtk_widget_add_css_class (edit, "settings-action");
  gtk_accessible_update_property (GTK_ACCESSIBLE (edit),
    GTK_ACCESSIBLE_PROPERTY_LABEL, "Edit keyboard shortcuts", -1);
  g_signal_connect (edit, "clicked", G_CALLBACK (edit_clicked), NULL);
  gtk_box_append (GTK_BOX (actions), edit);
  return actions;
}
