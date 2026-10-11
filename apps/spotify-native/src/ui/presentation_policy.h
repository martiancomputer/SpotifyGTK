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
         percent == 100 || percent == 150 || percent == 300;
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
  /* Ceil creates 1..2 physical pixels of artificial overflow at fractional
   * zoom even when the content fits. Only real minimum sizes may overflow. */
  return MAX (MAX (1, minimum), (int) MIN (floor (viewport / scale), G_MAXINT));
}
