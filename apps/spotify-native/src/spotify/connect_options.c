#include "connect_options.h"

#include "protobuf_min.h"

#include <string.h>

#define CTXOPT_SHUFFLING       1
#define CTXOPT_MODES           5
#define SETOPT_SHUFFLING       3
#define SETOPT_MODES           7
#define OPTIONAL_BOOL_VALUE    1
#define MAP_KEY                1
#define MAP_VALUE              2

/* Read context_enhancement from one protobuf map field. The same map occurs
 * at field 5 in ContextPlayerOptions and field 7 in SetOptionsRequest. */
static gboolean
options_enhancement (const guint8 *options, gsize len, guint modes_field,
                     gboolean *recommendation)
{
  gsize pos = 0;
  guint32 fn;
  PbWireType wt;
  const guint8 *field;
  gsize field_len;
  guint64 value;

  while (pb_read_field (options, len, &pos, &fn, &wt,
                        &field, &field_len, &value)) {
    if (fn != modes_field || wt != PB_WIRE_LENGTH_DELIMITED)
      continue;

    const guint8 *key = NULL, *val = NULL;
    gsize key_len = 0, val_len = 0;
    if (pb_find_bytes_field (field, field_len, MAP_KEY, &key, &key_len) &&
        pb_find_bytes_field (field, field_len, MAP_VALUE, &val, &val_len) &&
        key_len == strlen ("context_enhancement") &&
        memcmp (key, "context_enhancement", key_len) == 0) {
      *recommendation =
        val_len == strlen ("RECOMMENDATION") &&
        memcmp (val, "RECOMMENDATION", val_len) == 0;
      return TRUE;
    }
  }

  return FALSE;
}

guint
spotifygtk_connect_player_shuffle_mode (const guint8 *options, gsize len,
                                        guint fallback)
{
  guint64 shuffling = 0;
  gboolean has_shuffle = pb_find_varint_field (options, len,
                                                CTXOPT_SHUFFLING, &shuffling);
  gboolean recommendation = FALSE;
  gboolean has_enhancement = options_enhancement (options, len, CTXOPT_MODES,
                                                   &recommendation);

  if (recommendation)
    return 2;
  if (has_shuffle)
    return shuffling ? 1 : 0;
  if (has_enhancement && fallback == 2)
    return 1;
  return fallback;
}

guint
spotifygtk_connect_command_shuffle_mode (const guint8 *options, gsize len,
                                         guint fallback)
{
  /* A bare field 1 is retained for set_shuffling_context from older
   * controllers. In SetOptionsRequest field 1 is a message, so it cannot be
   * mistaken for this varint. */
  guint64 shuffling = 0;
  gboolean has_shuffle = pb_find_varint_field (options, len,
                                                CTXOPT_SHUFFLING, &shuffling);
  const guint8 *wrapped = NULL;
  gsize wrapped_len = 0;
  if (pb_find_bytes_field (options, len, SETOPT_SHUFFLING,
                           &wrapped, &wrapped_len))
    has_shuffle = pb_find_varint_field (wrapped, wrapped_len,
                                        OPTIONAL_BOOL_VALUE, &shuffling);

  gboolean recommendation = FALSE;
  gboolean has_enhancement = options_enhancement (options, len, SETOPT_MODES,
                                                   &recommendation);
  if (recommendation)
    return 2;
  if (has_shuffle)
    return shuffling ? 1 : 0;
  if (has_enhancement && fallback == 2)
    return 1;
  return fallback;
}
