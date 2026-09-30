#include "ui/page_stack.h"

static void
page_visibility (void)
{
  GtkStack *stack = GTK_STACK (gtk_stack_new ());
  g_object_ref_sink (stack);
  GtkWidget *first = gtk_label_new ("First");
  GtkWidget *second = gtk_label_new ("Second");
  GtkWidget *third = gtk_label_new ("Third");
  gtk_stack_add_named (stack, first, "first");
  gtk_stack_add_named (stack, second, "second");
  gtk_stack_add_named (stack, third, "third");
  spotifygtk_page_stack_hide_inactive (stack);
  g_assert_true (gtk_widget_get_visible (first));
  g_assert_false (gtk_widget_get_visible (second));
  g_assert_false (gtk_widget_get_visible (third));

  for (guint i = 0; i < 10; i++) {
    spotifygtk_page_stack_show (stack, "second");
    g_assert_true (gtk_stack_get_visible_child (stack) == second);
    spotifygtk_page_stack_hide_inactive (stack);
    g_assert_false (gtk_widget_get_visible (first));
    g_assert_true (gtk_widget_get_visible (second));
    spotifygtk_page_stack_show (stack, "first");
    spotifygtk_page_stack_hide_inactive (stack);
    g_assert_true (gtk_stack_get_visible_child (stack) == first);
  }
  g_object_unref (stack);
}

static void
crossfade (void)
{
  GtkWidget *window = gtk_window_new ();
  GtkStack *stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_set_transition_duration (stack, 100);
  GtkWidget *first = gtk_label_new ("First");
  GtkWidget *second = gtk_label_new ("Second");
  gtk_stack_add_named (stack, first, "first");
  gtk_stack_add_named (stack, second, "second");
  spotifygtk_page_stack_hide_inactive (stack);
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (stack));
  gtk_window_present (GTK_WINDOW (window));
  gint64 deadline = g_get_monotonic_time () + 1000000;
  while (!gtk_widget_get_mapped (first) && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, TRUE);
  g_assert_true (gtk_widget_get_mapped (first));
  spotifygtk_page_stack_show (stack, "second");
  g_assert_true (gtk_stack_get_transition_running (stack));
  spotifygtk_page_stack_hide_inactive (stack);
  g_assert_true (gtk_widget_get_visible (first));
  deadline = g_get_monotonic_time () + 2000000;
  while (gtk_stack_get_transition_running (stack) && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, TRUE);
  g_assert_false (gtk_stack_get_transition_running (stack));
  spotifygtk_page_stack_hide_inactive (stack);
  g_assert_false (gtk_widget_get_visible (first));
  g_assert_true (gtk_widget_get_visible (second));
  gtk_window_destroy (GTK_WINDOW (window));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  if (!gtk_init_check ()) {
    g_test_message ("GTK display unavailable; run under a display or Xvfb");
    return 77;
  }
  g_test_add_func ("/page-stack/inactive-visibility-and-revisit", page_visibility);
  g_test_add_func ("/page-stack/crossfade", crossfade);
  return g_test_run ();
}
