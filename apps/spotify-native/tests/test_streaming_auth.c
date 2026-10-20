/*
 * test_streaming_auth.c — offline tests for the clienttoken/login5
 * message encoding used by the streaming auth-relay chain.
 *
 * Same limitation as test_login_encoding.c: can't test actual server
 * acceptance from here (needs network + a real login5/clienttoken
 * round-trip), only that the messages we build match the schemas in
 * protocol/proto/spotify/{clienttoken,login5}/. That's still real
 * coverage -- it's exactly the class of bug (wrong field number,
 * missing field) that's bitten this project twice already in the AP
 * login path.
 */

#include <glib.h>
#include <string.h>
#include "spotify/protobuf_min.h"
#include "spotify/cdn.h"
#include "spotify/spclient.h"
#include "spotify/clienttoken.h"
#include "native_engine.h"
#include "audio/sink.h"
#if SPOTIFYGTK_TEST_VORBISENC
#include <vorbis/vorbisenc.h>
#endif

/* Exercise the production recovery state machine without credentials, Spotify
 * endpoints, real-time waits or an audio device. Only transport entry points
 * are replaced; buffering, seeking, retry policy and controls remain real. */
static guint storage_calls, range_calls, auth_calls, metadata_calls;
static goffset recovered_offset;
static gpointer pending_landing;
static guint64 fixture_seq, queued_pcm;
static SpotifyAudioSink *
fixture_sink (void)
{
  return (SpotifyAudioSink *)&fixture_seq;
}
static guint64
fixture_begin (SpotifyAudioSink *sink, SpotifyNativeEngineControl *control, GCancellable *cancel)
{
  (void)sink;
  (void)control;
  (void)cancel;
  return 1;
}
static gboolean
fixture_push (SpotifyAudioSink *sink, guint64 seq, PcmFrame *frame)
{
  queued_pcm += frame->n_frames;
  pcm_frame_free (frame);
  (void)sink;
  (void)seq;
  return TRUE;
}
static void
fixture_sink_action (SpotifyAudioSink *sink, guint64 seq)
{
  (void)sink;
  (void)seq;
}
static void
fixture_position (SpotifyAudioSink *sink, guint64 seq, guint64 frames)
{
  (void)sink;
  (void)seq;
  (void)frames;
}
static gint
fixture_rate (SpotifyAudioSink *sink)
{
  (void)sink;
  return 48000;
}
static gboolean
fixture_failed (SpotifyAudioSink *sink)
{
  (void)sink;
  return FALSE;
}
static void
fixture_storage (SpotifySpclient *client, const guint8 *id, gsize len, const gchar *bearer,
                 const gchar *token, SpclientStorageCallback callback, gpointer data)
{
  storage_calls++;
  (void)client;
  (void)id;
  (void)len;
  (void)bearer;
  (void)token;
  (void)callback;
  (void)data;
}
static void
fixture_range (SpotifyCdnFetcher *fetcher, const gchar *url, const guint8 key[AUDIO_KEY_LEN],
               goffset offset, gsize len, gpointer owner, CdnChunkCallback callback, gpointer data)
{
  range_calls++;
  recovered_offset = offset;
  if (data != owner)
    pending_landing = data;
  (void)fetcher;
  (void)url;
  (void)key;
  (void)len;
  (void)callback;
}
static void
fixture_auth (SpotifyClientToken *client, const gchar *id, const gchar *device,
              ClientTokenCallback callback, gpointer data)
{
  auth_calls++;
  (void)client;
  (void)id;
  (void)device;
  (void)callback;
  (void)data;
}
static void
fixture_metadata (SpotifySpclient *client, const gchar *uri, const gchar *bearer,
                  const gchar *token, SpclientTrackCallback callback, gpointer data)
{
  metadata_calls++;
  (void)client;
  (void)uri;
  (void)bearer;
  (void)token;
  (void)callback;
  (void)data;
}

#define SPOTIFYGTK_ENGINE_LIBRARY
#define spotifygtk_spclient_get_audio_storage fixture_storage
#define spotifygtk_cdn_fetch_chunk fixture_range
#define spotifygtk_client_token_request fixture_auth
#define spotifygtk_spclient_get_track_metadata fixture_metadata
#define spotifygtk_audio_sink_get fixture_sink
#define spotifygtk_audio_sink_begin_track fixture_begin
#define spotifygtk_audio_sink_push fixture_push
#define spotifygtk_audio_sink_end_track fixture_sink_action
#define spotifygtk_audio_sink_flush fixture_sink_action
#define spotifygtk_audio_sink_set_position fixture_position
#define spotifygtk_audio_sink_device_rate fixture_rate
#define spotifygtk_audio_sink_failed fixture_failed
#include "../src/main.c"
#undef spotifygtk_spclient_get_audio_storage
#undef spotifygtk_cdn_fetch_chunk
#undef spotifygtk_client_token_request
#undef spotifygtk_spclient_get_track_metadata
#undef spotifygtk_audio_sink_get
#undef spotifygtk_audio_sink_begin_track
#undef spotifygtk_audio_sink_push
#undef spotifygtk_audio_sink_end_track
#undef spotifygtk_audio_sink_flush
#undef spotifygtk_audio_sink_set_position
#undef spotifygtk_audio_sink_device_rate
#undef spotifygtk_audio_sink_failed

static gint engine_calls, engine_release;
static gint64 engine_seek, engine_position;
static gboolean engine_had_seek, engine_paused;
static guint abandon_calls;
static gboolean
fixture_engine (GCancellable *cancel, SpotifyNativeEngineProgressFunc progress, gpointer data,
                SpotifyNativeEngineControl *control, const gchar *uri)
{
  engine_position = spotifygtk_native_engine_control_get_position_ms (control);
  engine_had_seek = spotifygtk_native_engine_control_take_seek (control, &engine_seek);
  engine_paused = spotifygtk_native_engine_control_is_paused (control);
  spotifygtk_native_engine_control_set_sink_seq (control, 124);
  progress (SPOTIFYGTK_ENGINE_PLAYING, "Fixture PCM prepared", data);
  g_atomic_int_inc (&engine_calls);
  while (!g_atomic_int_get (&engine_release) && !g_cancellable_is_cancelled (cancel))
    g_usleep (1000);
  (void)uri;
  return FALSE;
}
static guint64
fixture_current (SpotifyAudioSink *sink)
{
  (void)sink;
  return fixture_seq;
}
static gboolean
fixture_done (SpotifyAudioSink *sink, guint64 seq)
{
  (void)sink;
  (void)seq;
  return FALSE;
}
static void
fixture_abandon (SpotifyAudioSink *sink)
{
  (void)sink;
  abandon_calls++;
}
#define spotifygtk_native_engine_run fixture_engine
#define spotifygtk_audio_sink_get fixture_sink
#define spotifygtk_audio_sink_current_seq fixture_current
#define spotifygtk_audio_sink_track_done fixture_done
#define spotifygtk_audio_sink_abandon_queued fixture_abandon
#include "../src/player_service.c"
#undef spotifygtk_native_engine_run
#undef spotifygtk_audio_sink_get
#undef spotifygtk_audio_sink_current_seq
#undef spotifygtk_audio_sink_track_done
#undef spotifygtk_audio_sink_abandon_queued

static void
recovery_setup (LiveTestState *state)
{
  memset (state, 0, sizeof *state);
  state->loop = g_main_loop_new (NULL, FALSE);
  state->control = spotifygtk_native_engine_control_new ();
  state->cancellable = g_cancellable_new ();
  state->spclient = spotifygtk_spclient_new ();
  state->stream_decoder = spotifygtk_decoder_new ();
  state->bearer_token = g_strdup ("fixture-bearer");
  state->client_token = g_strdup ("fixture-client");
  state->device_id = g_strdup ("fixture-device");
  state->next_download_offset = 1376256;
  spotifygtk_native_engine_control_report_position (state->control, 48000 * 120, 48000);
  storage_calls = range_calls = auth_calls = metadata_calls = 0;
  recovered_offset = -1;
  queued_pcm = 0;
  pending_landing = NULL;
  register_active_run (state);
  cred_cache_store ("fixture-bearer", "fixture-client", 3600);
}

static void
recovery_clear (LiveTestState *state)
{
  unregister_active_run (state);
  g_main_loop_unref (state->loop);
  spotifygtk_native_engine_control_free (state->control);
  g_clear_object (&state->cancellable);
  g_clear_object (&state->spclient);
  g_clear_object (&state->stream_decoder);
  g_clear_object (&state->client_token_client);
  g_clear_object (&state->login5_client);
  g_free (state->bearer_token);
  g_free (state->client_token);
  g_free (state->device_id);
  g_free (state->cdn_url);
  g_clear_pointer (&state->initial_cdn_chunk, g_bytes_unref);
  g_clear_pointer (&state->header_prefix, g_free);
  g_clear_pointer (&pending_landing, g_free);
  cred_cache_invalidate ();
}

static void
recover_with_url (LiveTestState *state)
{
  SpclientCdnUrls *urls = g_new0 (SpclientCdnUrls, 1);
  urls->n_urls = 1;
  urls->urls = g_new0 (gchar *, 2);
  urls->urls[0] = g_strdup ("https://example.invalid/renewed");
  on_read_ahead_restorage (urls, NULL, state);
}

static void
test_stream_recovery (gconstpointer data)
{
  LiveTestState state;
  recovery_setup (&state);
  SpotifyDecoder *decoder = state.stream_decoder;
  SpotifyNativeEngineControl *control = state.control;
  gboolean expired = GPOINTER_TO_UINT (data);
  if (expired)
    cred_cache_invalidate ();
  spotifygtk_native_engine_control_pause (control);
  g_autoptr (GError) timeout_error
      = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "fixture timeout");
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*read-ahead fetch failed*");
  on_read_ahead_chunk (NULL, state.next_download_offset, timeout_error, &state);
  g_test_assert_expected_messages ();
  if (expired)
    {
      g_assert_cmpuint (auth_calls, ==, 1);
      g_assert_cmpuint (storage_calls, ==, 0);
      on_login5_result ("fixture-renewed-bearer", 3600, NULL, &state);
      g_assert_cmpstr (state.bearer_token, ==, "fixture-renewed-bearer");
    }
  else
    {
      g_assert_cmpuint (auth_calls, ==, 0);
    }
  g_assert_cmpuint (storage_calls, ==, 1);
  /* A failed URL renewal must itself be retried, not restart the song. */
  on_read_ahead_restorage (NULL, timeout_error, &state);
  g_assert_cmpuint (storage_calls, ==, 2);
  recover_with_url (&state);
  g_assert_cmpuint (range_calls, ==, 1);
  g_assert_cmpint (recovered_offset, ==, 1376256);
  g_assert_true (state.stream_decoder == decoder);
  g_assert_true (state.control == control);
  g_assert_cmpint (spotifygtk_native_engine_control_get_position_ms (control), ==, 120000);
  g_assert_true (spotifygtk_native_engine_control_is_paused (control));
  g_assert_cmpuint (metadata_calls, ==, 0);
  recovery_clear (&state);
}

static void
test_recovery_refusal_is_bounded (void)
{
  LiveTestState state;
  recovery_setup (&state);
  g_autoptr (GError) refusal
      = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "fixture refusal");
  on_read_ahead_restorage (NULL, refusal, &state);
  g_assert_cmpuint (auth_calls, ==, 1);
  on_login5_result ("fixture-renewed-bearer", 3600, NULL, &state);
  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*could not re-resolve CDN storage*");
  on_read_ahead_restorage (NULL, refusal, &state);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (auth_calls, ==, 1);
  g_assert_cmpuint (range_calls, ==, 0);
  g_assert_cmpuint (metadata_calls, ==, 0);
  recovery_clear (&state);
}

static void
test_recovery_cancellation_and_stale_range (void)
{
  LiveTestState state;
  recovery_setup (&state);
  g_autoptr (GError) cancel
      = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "fixture cancellation");
  on_read_ahead_chunk (NULL, 0, cancel, &state);
  on_read_ahead_chunk (NULL, state.next_download_offset, cancel, &state);
  on_read_ahead_restorage (NULL, cancel, &state);
  g_assert_cmpuint (storage_calls + auth_calls + range_calls, ==, 0);
  recovery_clear (&state);
}

static void
test_resume_checkpoint (void)
{
  SpotifyNativeEngineControl *control = spotifygtk_native_engine_control_new ();
  spotifygtk_native_engine_control_report_position (control, 48000 * 120, 48000);
  spotifygtk_native_engine_control_report_buffered_position (control, 44100 * 150, 44100);
  g_assert_cmpint (spotifygtk_native_engine_control_get_resume_position_ms (control), ==, 150000);
  spotifygtk_native_engine_control_request_seek (control, 0);
  g_assert_cmpint (spotifygtk_native_engine_control_get_resume_position_ms (control), ==, 0);
  gint64 target;
  g_assert_true (spotifygtk_native_engine_control_take_seek (control, &target));
  g_assert_cmpint (spotifygtk_native_engine_control_get_resume_position_ms (control), ==, 0);
  spotifygtk_native_engine_control_report_buffered_position (control, 44100 * 30, 44100);
  spotifygtk_native_engine_control_report_position (control, 48000 * 5, 48000);
  g_assert_cmpint (spotifygtk_native_engine_control_get_resume_position_ms (control), ==, 30000);
  spotifygtk_native_engine_control_free (control);
}

#if SPOTIFYGTK_TEST_VORBISENC
static void
append_page (GByteArray *bytes, ogg_page *page)
{
  g_byte_array_append (bytes, page->header, page->header_len);
  g_byte_array_append (bytes, page->body, page->body_len);
}

static GBytes *
fixture_ogg (void)
{
  vorbis_info info;
  vorbis_comment comment;
  vorbis_dsp_state dsp;
  vorbis_block block;
  ogg_stream_state stream;
  ogg_packet id, comments, setup, packet;
  ogg_page page;
  vorbis_info_init (&info);
  g_assert_cmpint (vorbis_encode_init_vbr (&info, 1, 44100, 0.4f), ==, 0);
  vorbis_comment_init (&comment);
  g_assert_cmpint (vorbis_analysis_init (&dsp, &info), ==, 0);
  g_assert_cmpint (vorbis_block_init (&dsp, &block), ==, 0);
  g_assert_cmpint (ogg_stream_init (&stream, 1), ==, 0);
  g_autoptr (GByteArray) bytes = g_byte_array_new ();
  vorbis_analysis_headerout (&dsp, &comment, &id, &comments, &setup);
  ogg_stream_packetin (&stream, &id);
  ogg_stream_packetin (&stream, &comments);
  ogg_stream_packetin (&stream, &setup);
  while (ogg_stream_flush (&stream, &page))
    append_page (bytes, &page);
  guint32 random = 1;
  for (guint section = 0; section <= 44; section++)
    {
      if (section < 44)
        {
          float **pcm = vorbis_analysis_buffer (&dsp, 4096);
          for (guint i = 0; i < 4096; i++)
            {
              random = random * 1664525u + 1013904223u;
              pcm[0][i] = ((gfloat)(random >> 16) / 32768.0f - 1.0f) * 0.05f;
            }
          vorbis_analysis_wrote (&dsp, 4096);
        }
      else
        vorbis_analysis_wrote (&dsp, 0);
      while (vorbis_analysis_blockout (&dsp, &block) == 1)
        {
          vorbis_analysis (&block, NULL);
          vorbis_bitrate_addblock (&block);
          while (vorbis_bitrate_flushpacket (&dsp, &packet))
            {
              ogg_stream_packetin (&stream, &packet);
              while (ogg_stream_pageout (&stream, &page))
                append_page (bytes, &page);
            }
        }
    }
  while (ogg_stream_flush (&stream, &page))
    append_page (bytes, &page);
  ogg_stream_clear (&stream);
  vorbis_block_clear (&block);
  vorbis_dsp_clear (&dsp);
  vorbis_comment_clear (&comment);
  vorbis_info_clear (&info);
  return g_byte_array_free_to_bytes (g_steal_pointer (&bytes));
}
#endif

static void
test_initial_resume_seek (void)
{
#if SPOTIFYGTK_TEST_VORBISENC
  LiveTestState state;
  recovery_setup (&state);
  g_clear_object (&state.stream_decoder);
  state.initial_cdn_chunk = fixture_ogg ();
  g_assert_cmpuint (g_bytes_get_size (state.initial_cdn_chunk), >,
                    CDN_SEEK_PROBE_OFFSET + CDN_SEEK_PROBE_LENGTH);
  g_autoptr (GBytes) probe = g_bytes_new_from_bytes (state.initial_cdn_chunk, CDN_SEEK_PROBE_OFFSET,
                                                     CDN_SEEK_PROBE_LENGTH);
  spotifygtk_native_engine_control_report_position (state.control, 150000, 1000);
  spotifygtk_native_engine_control_request_seek (state.control, 150000);
  spotifygtk_native_engine_control_pause (state.control);
  on_cdn_seek_probe_result (probe, CDN_SEEK_PROBE_OFFSET, NULL, &state);
  g_assert_cmpuint (queued_pcm, ==, 0);
  g_assert_cmpuint (state.header_prefix_len, >, 0);
  g_assert_cmpuint (range_calls, ==, 1);
  g_assert_cmpint (recovered_offset, >, 0);
  g_assert_nonnull (pending_landing);
  g_assert_cmpint (((SeekLanding *)pending_landing)->target_ms, ==, 150000);
  g_assert_true (spotifygtk_native_engine_control_is_paused (state.control));
  g_assert_cmpint (spotifygtk_native_engine_control_get_position_ms (state.control), ==, 150000);
  recovery_clear (&state);
#else
  g_test_skip ("Vorbis encoder not available to generate a synthetic stream");
#endif
}

static void
count_now_playing (SpotifyNativePlayerService *self, const gchar *uri, gpointer data)
{
  (*(guint *)data)++;
  (void)self;
  (void)uri;
}

static void
test_player_retry (gconstpointer data)
{
  guint mode = GPOINTER_TO_UINT (data);
  g_autoptr (SpotifyNativePlayerService) player = spotifygtk_player_service_new ();
  player->track_uri = g_strdup ("spotify:track:fixture");
  player->control = spotifygtk_native_engine_control_new ();
  player->cancellable = g_cancellable_new ();
  spotifygtk_native_engine_control_report_position (player->control, 48000 * 120, 48000);
  spotifygtk_native_engine_control_report_buffered_position (player->control, 44100 * 150, 44100);
  spotifygtk_native_engine_control_set_sink_seq (player->control, 123);
  InflightTrack *old = g_new0 (InflightTrack, 1);
  old->uri = g_strdup (player->track_uri);
  old->control = spotifygtk_native_engine_control_ref (player->control);
  old->cancellable = g_object_ref (player->cancellable);
  g_ptr_array_add (player->inflight, old);
  player->audible_seq = fixture_seq = 123;
  if (mode == 5)
    {
      spotifygtk_native_engine_control_set_sink_seq (player->control, 0);
      player->audible_seq = fixture_seq = 0;
    }
  engine_calls = engine_release = 0;
  engine_seek = engine_position = -1;
  abandon_calls = 0;
  if (mode == 1)
    spotifygtk_player_service_pause (player);
  if (mode == 2)
    spotifygtk_native_engine_control_request_seek (player->control, 0);
  if (mode == 4)
    player->pending_uri = g_strdup ("spotify:track:replacement");
  g_autoptr (GTask) result = g_task_new (player, player->cancellable, NULL, NULL);
  player->task = g_object_ref (result);
  g_task_return_new_error (result, G_IO_ERROR, mode == 3 ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED,
                           "fixture failure");
  on_engine_finished (G_OBJECT (player), G_ASYNC_RESULT (result), g_object_ref (player));
  if (mode == 3)
    {
      g_assert_cmpint (g_atomic_int_get (&engine_calls), ==, 0);
      g_assert_null (player->pending_seek_uri);
      return;
    }
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (!g_atomic_int_get (&engine_calls))
    {
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
      while (g_main_context_iteration (NULL, FALSE))
        {
        }
      g_usleep (1000);
    }
  while (g_main_context_iteration (NULL, FALSE))
    {
    }
  g_assert_cmpint (engine_position, ==, mode == 2 || mode == 4 ? 0 : 150000);
  g_assert_cmpint (engine_had_seek, ==, mode != 4);
  g_assert_cmpint (engine_paused, ==, mode == 1);
  if (mode != 4)
    {
      guint changes = 0;
      gulong handler = g_signal_connect (player, "now-playing-changed",
                                         G_CALLBACK (count_now_playing), &changes);
      fixture_seq = 124;
      g_assert_nonnull (sync_audible_track (player));
      g_assert_cmpuint (changes, ==, mode == 5 ? 1 : 0);
      g_signal_handler_disconnect (player, handler);
    }
  if (mode == 1)
    {
      spotifygtk_player_service_resume (player);
      g_assert_false (spotifygtk_native_engine_control_is_paused (old->control));
      g_assert_false (spotifygtk_native_engine_control_is_paused (player->control));
    }
  if (mode == 2)
    g_assert_cmpuint (abandon_calls, ==, 1);
  g_atomic_int_set (&engine_release, 1);
  while (player->task)
    {
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
      while (g_main_context_iteration (NULL, FALSE))
        {
        }
      g_usleep (1000);
    }
  g_assert_cmpint (g_atomic_int_get (&engine_calls), ==, mode == 4 ? 2 : 1);
}

typedef struct {
  guint    calls;
  goffset  offset;
  gboolean cancelled;
} CdnCancelProbe;

static void
on_cancelled_chunk (GBytes *chunk, goffset offset, GError *error,
                    gpointer user_data)
{
  CdnCancelProbe *probe = user_data;
  g_assert_nonnull (probe);
  probe->calls++;
  probe->offset = offset;
  probe->cancelled = chunk == NULL &&
    g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_cdn_cancel_preserves_request_context (void)
{
  g_autoptr(SpotifyCdnFetcher) fetcher = spotifygtk_cdn_fetcher_new ();
  guint8 key[AUDIO_KEY_LEN] = { 0 };
  gint owner = 0;
  CdnCancelProbe probe = { 0 };
  CdnCancelProbe different = { 0 };

  /* No Spotify or external network required. Cancellation is synchronous,
   * before the local loopback request can return its own result. */
  spotifygtk_cdn_fetch_chunk (fetcher, "http://127.0.0.1:1/seek-test",
                              key, 4096, 16, &owner,
                              on_cancelled_chunk, &probe);
  g_assert_false (spotifygtk_cdn_fetcher_cancel_request (
    fetcher, &owner, on_cancelled_chunk, &different));
  g_assert_true (spotifygtk_cdn_fetcher_cancel_request (
    fetcher, &owner, on_cancelled_chunk, NULL));
  g_assert_cmpuint (probe.calls, ==, 1);
  g_assert_cmpint (probe.offset, ==, 4096);
  g_assert_true (probe.cancelled);

  /* A late libsoup completion must not notify the request a second time. */
  gint64 until = g_get_monotonic_time () + 500 * 1000;
  while (g_get_monotonic_time () < until) {
    while (g_main_context_iteration (NULL, FALSE)) {}
    g_usleep (1000);
  }
  g_assert_cmpuint (probe.calls, ==, 1);
}

/* Mirrors clienttoken.c's request construction. */
static GByteArray *
build_client_token_request (const gchar *client_version, const gchar *client_id)
{
  GByteArray *client_data = g_byte_array_new ();
  pb_write_bytes_field (client_data, 1, (const guint8 *) client_version, strlen (client_version));
  pb_write_bytes_field (client_data, 2, (const guint8 *) client_id, strlen (client_id));

  GByteArray *request = g_byte_array_new ();
  pb_write_varint_field  (request, 1, 1);  /* REQUEST_CLIENT_DATA_REQUEST */
  pb_write_message_field (request, 2, client_data->data, client_data->len);

  g_byte_array_free (client_data, TRUE);
  return request;
}

static void
test_client_token_request_shape (void)
{
  GByteArray *req = build_client_token_request ("1.2.52.442", "65b708073fc0480ea92a077233ca87bd");

  guint64 request_type = 0;
  g_assert_true (pb_find_varint_field (req->data, req->len, 1, &request_type));
  g_assert_cmpuint (request_type, ==, 1);

  const guint8 *cd_data = NULL; gsize cd_len = 0;
  g_assert_true (pb_find_bytes_field (req->data, req->len, 2, &cd_data, &cd_len));

  const guint8 *version_data = NULL; gsize version_len = 0;
  g_assert_true (pb_find_bytes_field (cd_data, cd_len, 1, &version_data, &version_len));
  g_assert_cmpmem (version_data, version_len, "1.2.52.442", 10);

  const guint8 *id_data = NULL; gsize id_len = 0;
  g_assert_true (pb_find_bytes_field (cd_data, cd_len, 2, &id_data, &id_len));
  g_assert_cmpmem (id_data, id_len, "65b708073fc0480ea92a077233ca87bd", 32);

  g_byte_array_free (req, TRUE);
}

/* Mirrors login5.c's request construction. */
static GByteArray *
build_login5_request (const gchar *client_id, const gchar *device_id,
                      const gchar *username, const guint8 *creds, gsize creds_len)
{
  GByteArray *client_info = g_byte_array_new ();
  pb_write_bytes_field (client_info, 1, (const guint8 *) client_id, strlen (client_id));
  pb_write_bytes_field (client_info, 2, (const guint8 *) device_id, strlen (device_id));

  GByteArray *stored_cred = g_byte_array_new ();
  if (username && *username)
    pb_write_bytes_field (stored_cred, 1, (const guint8 *) username, strlen (username));
  pb_write_bytes_field (stored_cred, 2, creds, creds_len);

  GByteArray *request = g_byte_array_new ();
  pb_write_message_field (request, 1,   client_info->data, client_info->len);
  pb_write_message_field (request, 100, stored_cred->data, stored_cred->len);

  g_byte_array_free (client_info, TRUE);
  g_byte_array_free (stored_cred, TRUE);
  return request;
}

static void
test_login5_request_shape_with_username (void)
{
  const guint8 fake_creds[] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02 };
  GByteArray *req = build_login5_request ("65b708073fc0480ea92a077233ca87bd", "test-device",
                                          "someuser", fake_creds, sizeof (fake_creds));

  const guint8 *ci_data = NULL; gsize ci_len = 0;
  g_assert_true (pb_find_bytes_field (req->data, req->len, 1, &ci_data, &ci_len));
  const guint8 *cid_data = NULL; gsize cid_len = 0;
  g_assert_true (pb_find_bytes_field (ci_data, ci_len, 1, &cid_data, &cid_len));
  g_assert_cmpmem (cid_data, cid_len, "65b708073fc0480ea92a077233ca87bd", 32);

  const guint8 *sc_data = NULL; gsize sc_len = 0;
  g_assert_true (pb_find_bytes_field (req->data, req->len, 100, &sc_data, &sc_len));

  const guint8 *user_data = NULL; gsize user_len = 0;
  g_assert_true (pb_find_bytes_field (sc_data, sc_len, 1, &user_data, &user_len));
  g_assert_cmpmem (user_data, user_len, "someuser", 8);

  const guint8 *creds_data = NULL; gsize creds_data_len = 0;
  g_assert_true (pb_find_bytes_field (sc_data, sc_len, 2, &creds_data, &creds_data_len));
  g_assert_cmpmem (creds_data, creds_data_len, fake_creds, sizeof (fake_creds));

  g_byte_array_free (req, TRUE);
}

static void
test_login5_request_omits_username_when_null (void)
{
  const guint8 fake_creds[] = { 0x01, 0x02, 0x03 };
  GByteArray *req = build_login5_request ("client-id", "device-id", NULL, fake_creds, sizeof (fake_creds));

  const guint8 *sc_data = NULL; gsize sc_len = 0;
  g_assert_true (pb_find_bytes_field (req->data, req->len, 100, &sc_data, &sc_len));

  const guint8 *user_data = NULL; gsize user_len = 0;
  g_assert_false (pb_find_bytes_field (sc_data, sc_len, 1, &user_data, &user_len));

  const guint8 *creds_data = NULL; gsize creds_data_len = 0;
  g_assert_true (pb_find_bytes_field (sc_data, sc_len, 2, &creds_data, &creds_data_len));
  g_assert_cmpmem (creds_data, creds_data_len, fake_creds, sizeof (fake_creds));

  g_byte_array_free (req, TRUE);
}

/* storage-resolve response parsing (repeated cdnurl field) --
 * verifies the pb_read_field-based manual walk in spclient.c against
 * a message with multiple repeated entries, since none of the
 * existing protobuf_min tests exercise a repeated field at all. */
static void
test_storage_resolve_repeated_cdnurl_parsing (void)
{
  GByteArray *response = g_byte_array_new ();
  pb_write_varint_field (response, 1, 0);  /* result = CDN */
  pb_write_bytes_field  (response, 2, (const guint8 *) "https://cdn1.example/a", 22);
  pb_write_bytes_field  (response, 2, (const guint8 *) "https://cdn2.example/b", 22);
  pb_write_bytes_field  (response, 2, (const guint8 *) "https://cdn3.example/c", 22);

  guint count = 0;
  gsize pos = 0;
  guint32 field_num;
  PbWireType wire_type;
  const guint8 *field_data;
  gsize field_len;
  guint64 field_varint;

  while (pb_read_field (response->data, response->len, &pos, &field_num, &wire_type,
                        &field_data, &field_len, &field_varint)) {
    if (field_num == 2 && wire_type == PB_WIRE_LENGTH_DELIMITED)
      count++;
  }

  g_assert_cmpuint (count, ==, 3);
  g_byte_array_free (response, TRUE);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/playback/recovery/signed-url", GUINT_TO_POINTER (0), test_stream_recovery);
  g_test_add_data_func ("/playback/recovery/expired-credentials", GUINT_TO_POINTER (1), test_stream_recovery);
  g_test_add_func ("/playback/recovery/refusal-bounded", test_recovery_refusal_is_bounded);
  g_test_add_func ("/playback/recovery/cancel-and-stale", test_recovery_cancellation_and_stale_range);
  g_test_add_func ("/playback/recovery/checkpoint", test_resume_checkpoint);
  g_test_add_func ("/playback/recovery/initial-seek-no-zero-pcm", test_initial_resume_seek);
  const gchar *retry_modes[] = { "buffered-tail", "paused", "seek-zero", "cancelled", "replacement", "before-first-pcm" };
  for (guint i = 0; i < G_N_ELEMENTS (retry_modes); i++) {
    g_autofree gchar *path = g_strconcat ("/playback/retry/", retry_modes[i], NULL);
    g_test_add_data_func (path, GUINT_TO_POINTER (i), test_player_retry);
  }
  g_test_add_func ("/streaming-auth/client-token-request-shape", test_client_token_request_shape);
  g_test_add_func ("/streaming-auth/login5-request-with-username", test_login5_request_shape_with_username);
  g_test_add_func ("/streaming-auth/login5-request-omits-username-when-null",
                   test_login5_request_omits_username_when_null);
  g_test_add_func ("/streaming-auth/storage-resolve-repeated-cdnurl",
                   test_storage_resolve_repeated_cdnurl_parsing);
  g_test_add_func ("/streaming-auth/cdn-cancel-preserves-request-context",
                   test_cdn_cancel_preserves_request_context);
  return g_test_run ();
}
