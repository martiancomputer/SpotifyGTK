#pragma once

#include <glib.h>
#include <math.h>

/* Pure validation/geometry policy; independent of GTK and displays. */
static inline gboolean
spotifygtk_presentation_valid_fps (guint fps)
{
  return fps == 0 || fps == 30 || fps == 60 || fps == 120 || fps == 144 ||
         fps == 240 || fps == 360;
}

static inline gboolean
spotifygtk_presentation_valid_scale (guint percent, gboolean custom)
{
  if (custom) return percent >= 25 && percent <= 300;
  return percent == 0 || percent == 25 || percent == 50 || percent == 75 ||
         percent == 90 || percent == 100 || percent == 150 || percent == 300;
}

static inline guint
spotifygtk_presentation_step_scale (guint percent, gboolean increase)
{
  int current = percent ? (int) MIN (percent, 300) : 100;
  return (guint) CLAMP (current + (increase ? 10 : -10), 25, 300);
}

static inline double
spotifygtk_presentation_scale (guint percent)
{
  return percent >= 25 && percent <= 300 ? percent / 100.0 : 1.0;
}

static inline int
spotifygtk_presentation_extent (int value, double scale)
{
  /* A decimal percentage can multiply to integer + one floating-point ULP
   * (300 * 1.37, for example). Do not invent an extra physical pixel. */
  return value < 0 ? -1 : (int) MIN (ceil (nextafter (value * scale, -INFINITY)), G_MAXINT);
}

static inline int
spotifygtk_presentation_logical_extent (int viewport, double scale, int minimum)
{
  /* Cover the final device pixel at fractional zoom. Overflow negotiation
   * uses the real child minimum, not this rounded-up allocation. */
  return MAX (MAX (1, minimum),
              (int) MIN (ceil (nextafter (viewport / scale, -INFINITY)), G_MAXINT));
}

/* Reveal the nearest edge of a focused widget. A large focus target that
 * already covers the viewport cannot be made wholly visible: moving to one
 * end would make repeated focus restoration jump between opposite ends. */
static inline double
spotifygtk_presentation_focus_delta (double start, double size, double viewport)
{
  if (!isfinite (start) || !isfinite (size) || !isfinite (viewport) ||
      size <= 0 || viewport <= 0)
    return 0;
  double end = start + size;
  if (size > viewport) {
    if (start <= 0 && end >= viewport) return 0;
    return start > 0 ? start : end - viewport;
  }
  return start < 0 ? start : MAX (0, end - viewport);
}
