/*
 * playlist.c — see playlist.h.
 */

#include "playlist.h"
#include "protobuf_min.h"
#include <string.h>

/* playlist4_external.proto field numbers. */
#define PL_SLC_REVISION    1
#define PL_SLC_LENGTH      2
#define PL_SLC_ATTRIBUTES  3
#define PL_SLC_CONTENTS    5
#define PL_LISTATTR_NAME   1
/* ListItems.items is 3 -- field 2 there is `truncated`. Add.items is 2. Two
 * different messages, two different numbers, and reusing one for the other
 * parses cleanly and finds nothing. */
#define PL_LISTITEMS_ITEMS 3
#define PL_ITEM_URI        1
#define PL_ADD_FROM_INDEX  1
#define PL_ADD_ITEMS       2
#define PL_ADD_ADD_LAST    4
#define PL_OP_KIND         1
#define PL_OP_ADD          2
#define PL_OP_KIND_ADD     2
/*
 * Removing a playlist is unfollowing it: an Op on the rootlist, not on the
 * playlist. Op.Kind is not consecutive -- ADD = 2 above is proven and
 * UPDATE_LIST_ATTRIBUTES = 6 below is proven, and the descriptor's order
 * (KIND_UNKNOWN, ADD, REM, MOV, UPDATE_ITEM_ATTRIBUTES, UPDATE_LIST_ATTRIBUTES,
 * UPDATE_ITEM_URIS) puts REM between them at 3.
 *
 * Rem is position-based: from_index and length, not a URI. So the rootlist has
 * to be read, the entry found by URI, and the removal posted against that
 * index -- and if the index is wrong a different playlist goes instead, which
 * is why remove() below refuses rather than guesses when it cannot find one.
 */
#define PL_OP_KIND_REM     3
#define PL_OP_REM          3
#define PL_REM_FROM_INDEX  1
#define PL_REM_LENGTH      2
#define PL_DELTA_OPS       2
#define PL_CHANGES_BASE    1
#define PL_CHANGES_DELTAS  2
#define PL_CREATEREPLY_URI 1

/* Naming is a separate op. POST hm://playlist/v2/playlist creates the list and
 * ignores any attributes in the body -- the result has an empty attributes
 * field -- so the name is set afterwards with UPDATE_LIST_ATTRIBUTES.
 *
 *   Op { kind = 6, update_list_attributes = 6 }
 *   UpdateListAttributes { new_attributes = 1 }
 *   ListAttributesPartialState { values = 1 }  -> ListAttributes { name = 1 }
 */
#define PL_OP_KIND_UPDATE_LIST  6
#define PL_OP_UPDATE_LIST       6
#define PL_ULA_NEW_ATTRIBUTES   1
#define PL_LAPS_VALUES          1

static gboolean
status_ok (MercuryResponse *r)
{
  return r && r->status_code >= 200 && r->status_code < 300;
}

/* "spotify:playlist:<id>" or "<id>" -> "<id>" */
static const gchar *
playlist_id_of (const gchar *uri)
{
  const gchar *last = strrchr (uri, ':');
  return last ? last + 1 : uri;
}

/* ── Create ──────────────────────────────────────────────────────────────── */

typedef struct {
  SpotifyMercury               *mercury;
  gchar                        *username;
  gchar                        *uri;        /* filled once created */
  gchar                        *name;
  SpotifyPlaylistCreateCallback callback;
  gpointer                      user_data;
} CreateCtx;

static void
create_ctx_free (CreateCtx *ctx)
{
  g_clear_object (&ctx->mercury);
  g_free (ctx->username);
  g_free (ctx->uri);
  g_free (ctx->name);
  g_free (ctx);
}

static void
on_name_set (MercuryResponse *response, gpointer user_data)
{
  CreateCtx *ctx = user_data;
  if (!status_ok (response))
    g_warning ("playlist: created and filed %s but naming it failed (status %d)",
               ctx->uri, response ? response->status_code : 0);

  /* Named or not, the playlist exists and is in the library, so this is a
   * success from the caller's point of view. */
  if (ctx->callback)
    ctx->callback (TRUE, response ? response->status_code : 200, ctx->uri,
                   ctx->user_data);
  create_ctx_free (ctx);
}

static void
on_named_head (MercuryResponse *response, gpointer user_data)
{
  CreateCtx *ctx = user_data;

  const guint8 *rev = NULL; gsize rev_len = 0;
  if (status_ok (response) && response->parts && response->parts->len > 0) {
    gsize hlen = 0;
    const guint8 *hd = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &hlen);
    pb_find_bytes_field (hd, hlen, PL_SLC_REVISION, &rev, &rev_len);
  }

  g_autoptr(GByteArray) attrs = g_byte_array_new ();
  pb_write_bytes_field (attrs, PL_LISTATTR_NAME,
                        (const guint8 *) ctx->name, strlen (ctx->name));

  g_autoptr(GByteArray) partial = g_byte_array_new ();
  pb_write_message_field (partial, PL_LAPS_VALUES, attrs->data, attrs->len);

  g_autoptr(GByteArray) ula = g_byte_array_new ();
  pb_write_message_field (ula, PL_ULA_NEW_ATTRIBUTES, partial->data, partial->len);

  g_autoptr(GByteArray) op = g_byte_array_new ();
  pb_write_varint_field (op, PL_OP_KIND, PL_OP_KIND_UPDATE_LIST);
  pb_write_message_field (op, PL_OP_UPDATE_LIST, ula->data, ula->len);

  g_autoptr(GByteArray) delta = g_byte_array_new ();
  if (rev) pb_write_bytes_field (delta, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (delta, PL_DELTA_OPS, op->data, op->len);

  g_autoptr(GByteArray) changes = g_byte_array_new ();
  if (rev) pb_write_bytes_field (changes, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (changes, PL_CHANGES_DELTAS, delta->data, delta->len);

  g_autofree gchar *uri = g_strdup_printf ("hm://playlist/v2/playlist/%s/changes",
                                           playlist_id_of (ctx->uri));
  g_autoptr(GBytes) body = g_bytes_new (changes->data, changes->len);
  spotifygtk_mercury_request_full (ctx->mercury, MERCURY_METHOD_SEND, "POST",
                                   uri, body, on_name_set, ctx);
}

static void
on_rootlist_filed (MercuryResponse *response, gpointer user_data)
{
  CreateCtx *ctx = user_data;
  gboolean ok = status_ok (response);

  /* The playlist exists either way; without this it is simply not in the
   * library, which is worth saying rather than reporting a clean success. */
  if (!ok)
    g_warning ("playlist: created %s but could not file it into the rootlist "
               "(status %d)", ctx->uri, response ? response->status_code : 0);

  if (!ok) {
    if (ctx->callback)
      ctx->callback (FALSE, response ? response->status_code : 0, ctx->uri,
                     ctx->user_data);
    create_ctx_free (ctx);
    return;
  }

  /* Filed; now give it its name. */
  g_autofree gchar *head = g_strdup_printf ("hm://playlist/v2/playlist/%s",
                                            playlist_id_of (ctx->uri));
  spotifygtk_mercury_request_full (ctx->mercury, MERCURY_METHOD_GET, "GET",
                                   head, NULL, on_named_head, ctx);
}

static void
on_rootlist_head (MercuryResponse *response, gpointer user_data)
{
  CreateCtx *ctx = user_data;

  if (!status_ok (response) || !response->parts || response->parts->len == 0) {
    if (ctx->callback)
      ctx->callback (FALSE, response ? response->status_code : 0, ctx->uri, ctx->user_data);
    create_ctx_free (ctx);
    return;
  }

  gsize hlen = 0;
  const guint8 *hd = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &hlen);
  const guint8 *rev = NULL; gsize rev_len = 0;
  pb_find_bytes_field (hd, hlen, PL_SLC_REVISION, &rev, &rev_len);

  g_autoptr(GByteArray) item = g_byte_array_new ();
  pb_write_bytes_field (item, PL_ITEM_URI, (const guint8 *) ctx->uri, strlen (ctx->uri));

  g_autoptr(GByteArray) add = g_byte_array_new ();
  pb_write_message_field (add, PL_ADD_ITEMS, item->data, item->len);
  /* add_last only. from_index is the alternative to it, and sending both is
   * refused with a bare 400 that names neither. */
  pb_write_varint_field (add, PL_ADD_ADD_LAST, 1);

  g_autoptr(GByteArray) op = g_byte_array_new ();
  pb_write_varint_field (op, PL_OP_KIND, PL_OP_KIND_ADD);
  pb_write_message_field (op, PL_OP_ADD, add->data, add->len);

  g_autoptr(GByteArray) delta = g_byte_array_new ();
  if (rev) pb_write_bytes_field (delta, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (delta, PL_DELTA_OPS, op->data, op->len);

  g_autoptr(GByteArray) changes = g_byte_array_new ();
  if (rev) pb_write_bytes_field (changes, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (changes, PL_CHANGES_DELTAS, delta->data, delta->len);

  g_autofree gchar *uri = g_strdup_printf (
    "hm://playlist/v2/user/%s/rootlist/changes", ctx->username);
  g_autoptr(GBytes) body = g_bytes_new (changes->data, changes->len);
  spotifygtk_mercury_request_full (ctx->mercury, MERCURY_METHOD_SEND, "POST",
                                   uri, body, on_rootlist_filed, ctx);
}

static void
on_create_reply (MercuryResponse *response, gpointer user_data)
{
  CreateCtx *ctx = user_data;

  if (!status_ok (response) || !response->parts || response->parts->len == 0) {
    if (ctx->callback)
      ctx->callback (FALSE, response ? response->status_code : 0, NULL, ctx->user_data);
    create_ctx_free (ctx);
    return;
  }

  gsize len = 0;
  const guint8 *d = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &len);
  const guint8 *uri = NULL; gsize ulen = 0;
  if (!pb_find_bytes_field (d, len, PL_CREATEREPLY_URI, &uri, &ulen)) {
    if (ctx->callback)
      ctx->callback (FALSE, response->status_code, NULL, ctx->user_data);
    create_ctx_free (ctx);
    return;
  }
  ctx->uri = g_strndup ((const gchar *) uri, ulen);

  /* Now file it, or it exists and is invisible. */
  g_autofree gchar *rl = g_strdup_printf ("hm://playlist/v2/user/%s/rootlist",
                                          ctx->username);
  spotifygtk_mercury_request_full (ctx->mercury, MERCURY_METHOD_GET, "GET",
                                   rl, NULL, on_rootlist_head, ctx);
}

void
spotifygtk_playlist_create (SpotifyMercury *mercury, const gchar *username,
                            const gchar *name,
                            SpotifyPlaylistCreateCallback callback,
                            gpointer user_data)
{
  g_return_if_fail (mercury != NULL && username != NULL && name != NULL);

  g_autoptr(GByteArray) attrs = g_byte_array_new ();
  pb_write_bytes_field (attrs, PL_LISTATTR_NAME, (const guint8 *) name, strlen (name));

  g_autoptr(GByteArray) body = g_byte_array_new ();
  pb_write_message_field (body, PL_SLC_ATTRIBUTES, attrs->data, attrs->len);

  CreateCtx *ctx = g_new0 (CreateCtx, 1);
  ctx->mercury   = g_object_ref (mercury);
  ctx->username  = g_strdup (username);
  ctx->name      = g_strdup (name);
  ctx->callback  = callback;
  ctx->user_data = user_data;

  g_autoptr(GBytes) payload = g_bytes_new (body->data, body->len);
  spotifygtk_mercury_request_full (mercury, MERCURY_METHOD_SEND, "POST",
                                   "hm://playlist/v2/playlist", payload,
                                   on_create_reply, ctx);
}

/* ── Add tracks ──────────────────────────────────────────────────────────── */

typedef struct {
  SpotifyMercury            *mercury;
  gchar                     *id;
  GPtrArray                 *uris;      /* owned gchar* */
  SpotifyPlaylistOpCallback  callback;
  gpointer                   user_data;
} AddCtx;

static void
add_ctx_free (AddCtx *ctx)
{
  g_clear_object (&ctx->mercury);
  g_free (ctx->id);
  g_clear_pointer (&ctx->uris, g_ptr_array_unref);
  g_free (ctx);
}

static void
on_add_done (MercuryResponse *response, gpointer user_data)
{
  AddCtx *ctx = user_data;
  if (ctx->callback)
    ctx->callback (status_ok (response), response ? response->status_code : 0,
                   ctx->user_data);
  add_ctx_free (ctx);
}

static void
on_add_head (MercuryResponse *response, gpointer user_data)
{
  AddCtx *ctx = user_data;

  if (!status_ok (response) || !response->parts || response->parts->len == 0) {
    if (ctx->callback)
      ctx->callback (FALSE, response ? response->status_code : 0, ctx->user_data);
    add_ctx_free (ctx);
    return;
  }

  gsize hlen = 0;
  const guint8 *hd = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &hlen);
  const guint8 *rev = NULL; gsize rev_len = 0;
  pb_find_bytes_field (hd, hlen, PL_SLC_REVISION, &rev, &rev_len);

  g_autoptr(GByteArray) add = g_byte_array_new ();
  for (guint i = 0; i < ctx->uris->len; i++) {
    const gchar *u = g_ptr_array_index (ctx->uris, i);
    g_autoptr(GByteArray) item = g_byte_array_new ();
    pb_write_bytes_field (item, PL_ITEM_URI, (const guint8 *) u, strlen (u));
    pb_write_message_field (add, PL_ADD_ITEMS, item->data, item->len);
  }
  pb_write_varint_field (add, PL_ADD_ADD_LAST, 1);

  g_autoptr(GByteArray) op = g_byte_array_new ();
  pb_write_varint_field (op, PL_OP_KIND, PL_OP_KIND_ADD);
  pb_write_message_field (op, PL_OP_ADD, add->data, add->len);

  g_autoptr(GByteArray) delta = g_byte_array_new ();
  if (rev) pb_write_bytes_field (delta, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (delta, PL_DELTA_OPS, op->data, op->len);

  g_autoptr(GByteArray) changes = g_byte_array_new ();
  if (rev) pb_write_bytes_field (changes, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (changes, PL_CHANGES_DELTAS, delta->data, delta->len);

  g_autofree gchar *uri =
    g_strdup_printf ("hm://playlist/v2/playlist/%s/changes", ctx->id);
  g_autoptr(GBytes) body = g_bytes_new (changes->data, changes->len);
  spotifygtk_mercury_request_full (ctx->mercury, MERCURY_METHOD_SEND, "POST",
                                   uri, body, on_add_done, ctx);
}

void
spotifygtk_playlist_add_tracks (SpotifyMercury *mercury, const gchar *playlist_uri,
                                const gchar *const *track_uris, guint n_tracks,
                                SpotifyPlaylistOpCallback callback,
                                gpointer user_data)
{
  g_return_if_fail (mercury != NULL && playlist_uri != NULL);
  if (n_tracks == 0) {
    if (callback) callback (TRUE, 200, user_data);
    return;
  }

  AddCtx *ctx = g_new0 (AddCtx, 1);
  ctx->mercury   = g_object_ref (mercury);
  ctx->id        = g_strdup (playlist_id_of (playlist_uri));
  ctx->uris      = g_ptr_array_new_with_free_func (g_free);
  ctx->callback  = callback;
  ctx->user_data = user_data;
  for (guint i = 0; i < n_tracks; i++)
    if (track_uris[i] && *track_uris[i])
      g_ptr_array_add (ctx->uris, g_strdup (track_uris[i]));

  /* Head first: the change must name the revision it is based on. */
  g_autofree gchar *head = g_strdup_printf ("hm://playlist/v2/playlist/%s", ctx->id);
  spotifygtk_mercury_request_full (mercury, MERCURY_METHOD_GET, "GET",
                                   head, NULL, on_add_head, ctx);
}

/* ── List ────────────────────────────────────────────────────────────────── */

typedef struct {
  SpotifyMercury             *mercury;
  gchar                      *username;
  guint                       attempt;
  SpotifyPlaylistListCallback callback;
  gpointer                    user_data;
} ListCtx;

static void request_rootlist (ListCtx *ctx);

void
spotifygtk_playlist_entries_free (SpotifyPlaylistEntry *entries, guint n)
{
  if (!entries) return;
  for (guint i = 0; i < n; i++) {
    g_free (entries[i].uri);
    g_free (entries[i].name);
  }
  g_free (entries);
}

static void
on_rootlist_read (MercuryResponse *response, gpointer user_data)
{
  ListCtx *ctx = user_data;

  if (response && response->status_code == 408 && ctx->attempt == 0) {
    ctx->attempt++;
    g_warning ("playlist: rootlist request timed out; retrying once");
    request_rootlist (ctx);
    return;
  }

  GArray *out = g_array_new (FALSE, TRUE, sizeof (SpotifyPlaylistEntry));

  if (status_ok (response) && response->parts && response->parts->len > 0) {
    gsize len = 0;
    const guint8 *d = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &len);

    /* SelectedListContent.contents -> ListItems.items -> Item.uri */
    const guint8 *contents = NULL; gsize clen = 0;
    if (pb_find_bytes_field (d, len, PL_SLC_CONTENTS, &contents, &clen)) {
      gsize pos = 0;
      guint32 fn; PbWireType wt; const guint8 *fd; gsize fl; guint64 fv;
      while (pb_read_field (contents, clen, &pos, &fn, &wt, &fd, &fl, &fv)) {
        if (fn != PL_LISTITEMS_ITEMS || wt != PB_WIRE_LENGTH_DELIMITED)
          continue;
        const guint8 *u = NULL; gsize ul = 0;
        if (!pb_find_bytes_field (fd, fl, PL_ITEM_URI, &u, &ul))
          continue;
        SpotifyPlaylistEntry e = { 0 };
        e.uri = g_strndup ((const gchar *) u, ul);
        g_array_append_val (out, e);
      }
    }
  }

  if (ctx->callback)
    ctx->callback (status_ok (response), response ? response->status_code : 0,
                   (SpotifyPlaylistEntry *) out->data, out->len, ctx->user_data);

  for (guint i = 0; i < out->len; i++)
    g_free (g_array_index (out, SpotifyPlaylistEntry, i).uri);
  g_array_free (out, TRUE);
  g_clear_object (&ctx->mercury);
  g_free (ctx->username);
  g_free (ctx);
}

static void
request_rootlist (ListCtx *ctx)
{
  g_autofree gchar *uri =
    g_strdup_printf ("hm://playlist/v2/user/%s/rootlist", ctx->username);
  spotifygtk_mercury_request_full (ctx->mercury, MERCURY_METHOD_GET, "GET",
                                   uri, NULL, on_rootlist_read, ctx);
}

/* ── Removing (unfollowing) a playlist ─────────────────────────────────── */

typedef struct {
  SpotifyMercury            *mercury;
  gchar                     *username;
  gchar                     *uri;
  SpotifyPlaylistOpCallback  callback;
  gpointer                   user_data;
} RemoveCtx;

static void
remove_ctx_free (RemoveCtx *ctx)
{
  g_clear_object (&ctx->mercury);
  g_free (ctx->username);
  g_free (ctx->uri);
  g_free (ctx);
}

static void
on_rootlist_removed (MercuryResponse *response, gpointer user_data)
{
  RemoveCtx *ctx = user_data;
  gboolean ok = status_ok (response);
  if (!ok)
    g_warning ("playlist: removing %s from the rootlist failed (status %d)",
               ctx->uri, response ? response->status_code : 0);
  if (ctx->callback)
    ctx->callback (ok, response ? response->status_code : 0, ctx->user_data);
  remove_ctx_free (ctx);
}

static void
on_remove_rootlist_head (MercuryResponse *response, gpointer user_data)
{
  RemoveCtx *ctx = user_data;

  if (!status_ok (response) || !response->parts || response->parts->len == 0) {
    if (ctx->callback)
      ctx->callback (FALSE, response ? response->status_code : 0, ctx->user_data);
    remove_ctx_free (ctx);
    return;
  }

  gsize len = 0;
  const guint8 *d = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &len);

  const guint8 *rev = NULL; gsize rev_len = 0;
  pb_find_bytes_field (d, len, PL_SLC_REVISION, &rev, &rev_len);

  /* Find the entry's position. The op names an index, so this is the whole
   * safety of the operation: no match means do nothing at all. */
  gint index = -1;
  guint seen = 0;
  const guint8 *contents = NULL; gsize clen = 0;
  if (pb_find_bytes_field (d, len, PL_SLC_CONTENTS, &contents, &clen)) {
    gsize pos = 0; guint32 fn; PbWireType wt;
    const guint8 *fd; gsize fl; guint64 fv;
    while (pb_read_field (contents, clen, &pos, &fn, &wt, &fd, &fl, &fv)) {
      if (fn != PL_LISTITEMS_ITEMS || wt != PB_WIRE_LENGTH_DELIMITED)
        continue;
      const guint8 *u = NULL; gsize ul = 0;
      if (pb_find_bytes_field (fd, fl, PL_ITEM_URI, &u, &ul)) {
        g_autofree gchar *entry = g_strndup ((const gchar *) u, ul);
        if (g_strcmp0 (entry, ctx->uri) == 0 && index < 0)
          index = (gint) seen;
      }
      seen++;
    }
  }

  if (index < 0) {
    g_warning ("playlist: %s is not in the rootlist (%u entries); not removing "
               "anything", ctx->uri, seen);
    if (ctx->callback)
      ctx->callback (FALSE, 404, ctx->user_data);
    remove_ctx_free (ctx);
    return;
  }

  g_message ("playlist: unfollowing %s at rootlist index %d of %u",
             ctx->uri, index, seen);

  g_autoptr(GByteArray) rem = g_byte_array_new ();
  pb_write_varint_field (rem, PL_REM_FROM_INDEX, (guint64) index);
  pb_write_varint_field (rem, PL_REM_LENGTH, 1);

  g_autoptr(GByteArray) op = g_byte_array_new ();
  pb_write_varint_field (op, PL_OP_KIND, PL_OP_KIND_REM);
  pb_write_message_field (op, PL_OP_REM, rem->data, rem->len);

  g_autoptr(GByteArray) delta = g_byte_array_new ();
  if (rev) pb_write_bytes_field (delta, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (delta, PL_DELTA_OPS, op->data, op->len);

  g_autoptr(GByteArray) changes = g_byte_array_new ();
  if (rev) pb_write_bytes_field (changes, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (changes, PL_CHANGES_DELTAS, delta->data, delta->len);

  g_autofree gchar *uri = g_strdup_printf (
    "hm://playlist/v2/user/%s/rootlist/changes", ctx->username);
  g_autoptr(GBytes) body = g_bytes_new (changes->data, changes->len);
  spotifygtk_mercury_request_full (ctx->mercury, MERCURY_METHOD_SEND, "POST",
                                   uri, body, on_rootlist_removed, ctx);
}


/* ── Renaming ───────────────────────────────────────────────────────────── */

typedef struct {
  SpotifyMercury           *mercury;
  gchar                    *id;
  gchar                    *name;
  SpotifyPlaylistOpCallback callback;
  gpointer                  user_data;
} RenameCtx;

static void
rename_ctx_free (RenameCtx *ctx)
{
  g_clear_object (&ctx->mercury);
  g_free (ctx->id);
  g_free (ctx->name);
  g_free (ctx);
}

static void
on_renamed (MercuryResponse *response, gpointer user_data)
{
  RenameCtx *ctx = user_data;
  gboolean ok = status_ok (response);
  if (!ok)
    g_warning ("playlist: renaming %s failed (status %d)",
               ctx->id, response ? response->status_code : 0);
  if (ctx->callback)
    ctx->callback (ok, response ? response->status_code : 0, ctx->user_data);
  rename_ctx_free (ctx);
}

static void
on_rename_head (MercuryResponse *response, gpointer user_data)
{
  RenameCtx *ctx = user_data;

  const guint8 *rev = NULL; gsize rev_len = 0;
  if (status_ok (response) && response->parts && response->parts->len > 0) {
    gsize hlen = 0;
    const guint8 *hd = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &hlen);
    pb_find_bytes_field (hd, hlen, PL_SLC_REVISION, &rev, &rev_len);
  }

  g_message ("playlist: renaming %s to \"%s\"", ctx->id, ctx->name);

  g_autoptr(GByteArray) attrs = g_byte_array_new ();
  pb_write_bytes_field (attrs, PL_LISTATTR_NAME,
                        (const guint8 *) ctx->name, strlen (ctx->name));

  g_autoptr(GByteArray) partial = g_byte_array_new ();
  pb_write_message_field (partial, PL_LAPS_VALUES, attrs->data, attrs->len);

  g_autoptr(GByteArray) ula = g_byte_array_new ();
  pb_write_message_field (ula, PL_ULA_NEW_ATTRIBUTES, partial->data, partial->len);

  g_autoptr(GByteArray) op = g_byte_array_new ();
  pb_write_varint_field (op, PL_OP_KIND, PL_OP_KIND_UPDATE_LIST);
  pb_write_message_field (op, PL_OP_UPDATE_LIST, ula->data, ula->len);

  g_autoptr(GByteArray) delta = g_byte_array_new ();
  if (rev) pb_write_bytes_field (delta, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (delta, PL_DELTA_OPS, op->data, op->len);

  g_autoptr(GByteArray) changes = g_byte_array_new ();
  if (rev) pb_write_bytes_field (changes, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (changes, PL_CHANGES_DELTAS, delta->data, delta->len);

  g_autofree gchar *uri =
    g_strdup_printf ("hm://playlist/v2/playlist/%s/changes", ctx->id);
  g_autoptr(GBytes) body = g_bytes_new (changes->data, changes->len);
  spotifygtk_mercury_request_full (ctx->mercury, MERCURY_METHOD_SEND, "POST",
                                   uri, body, on_renamed, ctx);
}

void
spotifygtk_playlist_rename (SpotifyMercury            *mercury,
                            const gchar               *playlist_uri,
                            const gchar               *new_name,
                            SpotifyPlaylistOpCallback  callback,
                            gpointer                   user_data)
{
  g_return_if_fail (mercury != NULL);
  g_return_if_fail (playlist_uri != NULL && new_name != NULL);

  RenameCtx *ctx = g_new0 (RenameCtx, 1);
  ctx->mercury   = g_object_ref (mercury);
  ctx->id        = g_strdup (playlist_id_of (playlist_uri));
  ctx->name      = g_strdup (new_name);
  ctx->callback  = callback;
  ctx->user_data = user_data;

  /* Head first: the change names the revision it is based on. */
  g_autofree gchar *head = g_strdup_printf ("hm://playlist/v2/playlist/%s", ctx->id);
  spotifygtk_mercury_request_full (mercury, MERCURY_METHOD_GET, "GET",
                                   head, NULL, on_rename_head, ctx);
}

/* ── Removing one track from a playlist ─────────────────────────────────── */

typedef struct {
  SpotifyMercury           *mercury;
  gchar                    *id;        /* bare playlist id */
  gchar                    *track_uri;
  gint                      expected;  /* row the caller meant, or -1 */
  SpotifyPlaylistOpCallback callback;
  gpointer                  user_data;
} RemoveTrackCtx;

static void
remove_track_ctx_free (RemoveTrackCtx *ctx)
{
  g_clear_object (&ctx->mercury);
  g_free (ctx->id);
  g_free (ctx->track_uri);
  g_free (ctx);
}

static void
on_track_removed (MercuryResponse *response, gpointer user_data)
{
  RemoveTrackCtx *ctx = user_data;
  gboolean ok = status_ok (response);
  if (!ok)
    g_warning ("playlist: removing %s from %s failed (status %d)",
               ctx->track_uri, ctx->id, response ? response->status_code : 0);
  if (ctx->callback)
    ctx->callback (ok, response ? response->status_code : 0, ctx->user_data);
  remove_track_ctx_free (ctx);
}

static void
on_remove_track_head (MercuryResponse *response, gpointer user_data)
{
  RemoveTrackCtx *ctx = user_data;

  if (!status_ok (response) || !response->parts || response->parts->len == 0) {
    if (ctx->callback)
      ctx->callback (FALSE, response ? response->status_code : 0, ctx->user_data);
    remove_track_ctx_free (ctx);
    return;
  }

  gsize len = 0;
  const guint8 *d = g_bytes_get_data (g_ptr_array_index (response->parts, 0), &len);

  const guint8 *rev = NULL; gsize rev_len = 0;
  pb_find_bytes_field (d, len, PL_SLC_REVISION, &rev, &rev_len);

  /*
   * Collect every position holding this track, rather than stopping at the
   * first. A playlist is allowed to contain the same track twice, and "the
   * first one" is not what the listener right-clicked.
   */
  g_autoptr(GArray) matches = g_array_new (FALSE, FALSE, sizeof (guint));
  guint seen = 0;
  const guint8 *contents = NULL; gsize clen = 0;
  if (pb_find_bytes_field (d, len, PL_SLC_CONTENTS, &contents, &clen)) {
    gsize pos = 0; guint32 fn; PbWireType wt;
    const guint8 *fd; gsize fl; guint64 fv;
    while (pb_read_field (contents, clen, &pos, &fn, &wt, &fd, &fl, &fv)) {
      if (fn != PL_LISTITEMS_ITEMS || wt != PB_WIRE_LENGTH_DELIMITED)
        continue;
      const guint8 *u = NULL; gsize ul = 0;
      if (pb_find_bytes_field (fd, fl, PL_ITEM_URI, &u, &ul)) {
        g_autofree gchar *entry = g_strndup ((const gchar *) u, ul);
        if (g_strcmp0 (entry, ctx->track_uri) == 0)
          g_array_append_val (matches, seen);
      }
      seen++;
    }
  }

  /*
   * Prefer the row the caller named, once the server agrees that row holds
   * this track. Failing that, one match is unambiguous and is taken. Several
   * matches with no usable position is exactly the case where guessing removes
   * the wrong copy, so it does nothing instead.
   */
  gint index = -1;
  for (guint i = 0; i < matches->len; i++)
    if ((gint) g_array_index (matches, guint, i) == ctx->expected)
      index = ctx->expected;

  if (index < 0 && matches->len == 1)
    index = (gint) g_array_index (matches, guint, 0);

  if (index < 0) {
    if (matches->len == 0)
      g_warning ("playlist: %s is not in %s (%u items); removing nothing",
                 ctx->track_uri, ctx->id, seen);
    else
      g_warning ("playlist: %s appears %u times in %s and row %d is not one of "
                 "them; removing nothing rather than the wrong copy",
                 ctx->track_uri, matches->len, ctx->id, ctx->expected);
    if (ctx->callback)
      ctx->callback (FALSE, 409, ctx->user_data);
    remove_track_ctx_free (ctx);
    return;
  }

  g_message ("playlist: removing %s from %s at index %d of %u",
             ctx->track_uri, ctx->id, index, seen);

  g_autoptr(GByteArray) rem = g_byte_array_new ();
  pb_write_varint_field (rem, PL_REM_FROM_INDEX, (guint64) index);
  pb_write_varint_field (rem, PL_REM_LENGTH, 1);

  g_autoptr(GByteArray) op = g_byte_array_new ();
  pb_write_varint_field (op, PL_OP_KIND, PL_OP_KIND_REM);
  pb_write_message_field (op, PL_OP_REM, rem->data, rem->len);

  g_autoptr(GByteArray) delta = g_byte_array_new ();
  if (rev) pb_write_bytes_field (delta, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (delta, PL_DELTA_OPS, op->data, op->len);

  g_autoptr(GByteArray) changes = g_byte_array_new ();
  if (rev) pb_write_bytes_field (changes, PL_CHANGES_BASE, rev, rev_len);
  pb_write_message_field (changes, PL_CHANGES_DELTAS, delta->data, delta->len);

  g_autofree gchar *uri =
    g_strdup_printf ("hm://playlist/v2/playlist/%s/changes", ctx->id);
  g_autoptr(GBytes) body = g_bytes_new (changes->data, changes->len);
  spotifygtk_mercury_request_full (ctx->mercury, MERCURY_METHOD_SEND, "POST",
                                   uri, body, on_track_removed, ctx);
}

void
spotifygtk_playlist_remove_track (SpotifyMercury            *mercury,
                                  const gchar               *playlist_uri,
                                  const gchar               *track_uri,
                                  gint                       expected_index,
                                  SpotifyPlaylistOpCallback  callback,
                                  gpointer                   user_data)
{
  g_return_if_fail (mercury != NULL);
  g_return_if_fail (playlist_uri != NULL && track_uri != NULL);

  RemoveTrackCtx *ctx = g_new0 (RemoveTrackCtx, 1);
  ctx->mercury   = g_object_ref (mercury);
  ctx->id        = g_strdup (playlist_id_of (playlist_uri));
  ctx->track_uri = g_strdup (track_uri);
  ctx->expected  = expected_index;
  ctx->callback  = callback;
  ctx->user_data = user_data;

  /* Head first, for the revision the change is based on and the positions. */
  g_autofree gchar *head = g_strdup_printf ("hm://playlist/v2/playlist/%s", ctx->id);
  spotifygtk_mercury_request_full (mercury, MERCURY_METHOD_GET, "GET",
                                   head, NULL, on_remove_track_head, ctx);
}

void
spotifygtk_playlist_remove (SpotifyMercury            *mercury,
                            const gchar               *username,
                            const gchar               *playlist_uri,
                            SpotifyPlaylistOpCallback  callback,
                            gpointer                   user_data)
{
  g_return_if_fail (mercury != NULL);
  g_return_if_fail (username != NULL && playlist_uri != NULL);

  RemoveCtx *ctx = g_new0 (RemoveCtx, 1);
  ctx->mercury   = g_object_ref (mercury);
  ctx->username  = g_strdup (username);
  ctx->uri       = g_strdup (playlist_uri);
  ctx->callback  = callback;
  ctx->user_data = user_data;

  g_autofree gchar *rl = g_strdup_printf ("hm://playlist/v2/user/%s/rootlist",
                                          username);
  spotifygtk_mercury_request_full (mercury, MERCURY_METHOD_GET, "GET", rl, NULL,
                                   on_remove_rootlist_head, ctx);
}

void
spotifygtk_playlist_list (SpotifyMercury *mercury, const gchar *username,
                          SpotifyPlaylistListCallback callback, gpointer user_data)
{
  g_return_if_fail (mercury != NULL && username != NULL);

  ListCtx *ctx = g_new0 (ListCtx, 1);
  ctx->mercury = g_object_ref (mercury);
  ctx->username = g_strdup (username);
  ctx->callback  = callback;
  ctx->user_data = user_data;
  request_rootlist (ctx);
}
