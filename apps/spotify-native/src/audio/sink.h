/*
 * sink.h — The audio device, kept open across tracks.
 *
 * Playback used to own its output: each track's engine run opened a device on
 * its first decoded frame and drained and closed it on its last. That is what
 * made the gap between tracks, and the gap was not one thing but three -- the
 * drain waiting for the device buffer to empty, the close, and then a cold
 * start of the next track (resolve, audio key, first CDN range, first decode)
 * before a device could be opened again.
 *
 * So the device moves here, above any single track, and the queue with it.
 *
 * The sink holds tracks in submission order, each with its own frame queue.
 * The writer plays the head track until it is both ended and empty, then moves
 * to the next. That ordering is the point: it lets a second track be decoded
 * and queued while the first is still playing, without the two interleaving.
 * Producers only ever touch their own track's queue.
 *
 * Nothing here decodes or fetches. It is handed PCM and a control, and its
 * only job is to keep the device fed without ever letting it run dry between
 * tracks.
 */

#pragma once

#include <glib.h>
#include <gio/gio.h>

#include "decoder.h"
#include "../native_engine.h"

G_BEGIN_DECLS

typedef struct _SpotifyAudioSink SpotifyAudioSink;

/*
 * The process-wide sink. Created on first use and kept for the life of the
 * process, like the cover loader -- a device that is closed and reopened per
 * track is the thing being fixed, so its lifetime cannot be tied to one.
 */
SpotifyAudioSink *spotifygtk_audio_sink_get (void);

/*
 * Claim the next slot in the play order.
 *
 * Called before any frame is pushed, and before the previous track has
 * finished playing -- that is what makes the handover seamless. The returned
 * sequence identifies this track in every other call here.
 *
 * The sink takes its own reference on `control` and holds it until the track's
 * last frame has been written, so the caller is free to let go as soon as it
 * has finished producing.
 */
guint64 spotifygtk_audio_sink_begin_track (SpotifyAudioSink *self,
                                           SpotifyNativeEngineControl *control,
                                           GCancellable *cancellable);

/*
 * Hand over one decoded frame. Takes ownership.
 *
 * Blocks while this track already has more than its share of audio buffered,
 * which is what stops a decoder from running arbitrarily far ahead. Returns
 * FALSE if the frame was rejected -- the track was flushed, cancelled, or the
 * device has failed -- in which case it has already been freed and the caller
 * should stop producing.
 */
gboolean spotifygtk_audio_sink_push (SpotifyAudioSink *self, guint64 seq,
                                     PcmFrame *frame);

/* No more frames for this track. The writer moves on once it has played what
 * is already queued. */
void spotifygtk_audio_sink_end_track (SpotifyAudioSink *self, guint64 seq);

/*
 * Drop whatever is queued for this track and let its producer through.
 *
 * For a seek, where everything buffered is audio from the old position, and
 * for a skip. Does not end the track: the producer may keep pushing from the
 * new position.
 */
void spotifygtk_audio_sink_flush (SpotifyAudioSink *self, guint64 seq);

/* How many frames this track still has queued ahead of the device. The
 * prefetch decision reads this: it is the real measure of how much playing
 * time is left, which frames-decoded is not. */
guint64 spotifygtk_audio_sink_queued_frames (SpotifyAudioSink *self, guint64 seq);

/* TRUE once this track's frames are all written and the writer has moved on. */
gboolean spotifygtk_audio_sink_track_done (SpotifyAudioSink *self, guint64 seq);

/* The device could not be opened, or a write failed. Playback cannot proceed. */
gboolean spotifygtk_audio_sink_failed (SpotifyAudioSink *self);

/* Rate the device is actually running at, or 0 before it has opened. Position
 * is reported in these frames, not the stream's. */
gint spotifygtk_audio_sink_device_rate (SpotifyAudioSink *self);

/*
 * Stop the writer and close the device.
 *
 * Only for shutdown. Calling this between tracks is exactly the behaviour this
 * file exists to remove.
 */
void spotifygtk_audio_sink_shutdown (SpotifyAudioSink *self);

G_END_DECLS
