/*
 * output_wasapi.c — Windows audio output (WASAPI).
 *
 * Shared mode, not exclusive: a music player gains nothing from exclusive mode
 * beyond locking every other application out of the device, and shared mode is
 * what lets the system mixer coexist. The stream is opened with
 * AUTOCONVERT_PCM so Windows will rate-convert if the endpoint's mix format
 * disagrees with the stream — without it Initialize() simply fails whenever the
 * mixer runs at 48 kHz and the track decodes at 44.1 kHz, which is the common
 * case. Our own resampler still decides the rate handed down here; this flag is
 * only the safety net for when the device refuses it anyway.
 *
 * Same vtable shape as output_pulse.c / output_alsa.c, so nothing above this
 * file changes — the output abstraction was written for this from the start.
 *
 * COM apartments are per-thread. try_open, write, drain and close are all
 * called from the engine's audio worker (audio_output_thread in main.c), so the
 * CoInitializeEx here pairs with the CoUninitialize in close on that same
 * thread, which is what COM requires.
 */

#include "config.h"
#include "output.h"

#ifdef G_OS_WIN32

/* INITGUID makes the DEFINE_GUID declarations in the headers below emit real
 * definitions in this translation unit, so CLSID_MMDeviceEnumerator and friends
 * resolve without linking a separate uuid library. COBJMACROS gives the
 * IFoo_Method(ptr, ...) call form, since C has no method-call syntax. */
#define INITGUID
#define COBJMACROS

#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <objbase.h>
#include <string.h>

/* Older mingw-w64 headers predate these; the values are stable ABI. */
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERT_PCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERT_PCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

/* 100 ms of buffer, in REFERENCE_TIME (100-nanosecond) units: enough that a
 * scheduling hiccup upstream does not underrun, short enough that stop and
 * track switches stay responsive. */
#define WASAPI_BUFFER_100NS  1000000

typedef struct {
  IMMDeviceEnumerator *enumerator;
  IMMDevice           *device;
  IAudioClient        *client;
  IAudioRenderClient  *render;
  ISimpleAudioVolume  *volume;

  UINT32   buffer_frames;
  gint     channels;
  gboolean com_initialised;
  gboolean started;
} WasapiData;

static void
wasapi_release (WasapiData *d)
{
  if (!d)
    return;

  if (d->client && d->started)
    IAudioClient_Stop (d->client);
  if (d->volume)     ISimpleAudioVolume_Release (d->volume);
  if (d->render)     IAudioRenderClient_Release (d->render);
  if (d->client)     IAudioClient_Release (d->client);
  if (d->device)     IMMDevice_Release (d->device);
  if (d->enumerator) IMMDeviceEnumerator_Release (d->enumerator);

  if (d->com_initialised)
    CoUninitialize ();

  g_free (d);
}

/*
 * Blocking write, matching what the other backends present: return only once
 * every frame has been handed to the device. The audio worker is a dedicated
 * thread, so blocking here is the backpressure that paces decoding — the same
 * role snd_pcm_writei plays on ALSA.
 */
static gsize
wasapi_write (SpotifyAudioOutput *self, const gint16 *samples, gsize n_frames)
{
  WasapiData *d = self->backend_data;
  if (!d || !d->render || !d->client)
    return 0;

  gsize written = 0;

  while (written < n_frames) {
    UINT32 padding = 0;
    if (FAILED (IAudioClient_GetCurrentPadding (d->client, &padding)))
      break;

    UINT32 avail = d->buffer_frames - padding;
    if (avail == 0) {
      Sleep (5);              /* buffer full; wait rather than spin */
      continue;
    }

    UINT32 chunk = (UINT32) MIN ((gsize) avail, n_frames - written);
    BYTE  *dst   = NULL;
    if (FAILED (IAudioRenderClient_GetBuffer (d->render, chunk, &dst)) || !dst)
      break;

    memcpy (dst, samples + written * (gsize) d->channels,
            (gsize) chunk * (gsize) d->channels * sizeof (gint16));

    if (FAILED (IAudioRenderClient_ReleaseBuffer (d->render, chunk, 0)))
      break;

    written += chunk;
  }

  return written;
}

static void
wasapi_set_volume (SpotifyAudioOutput *self, gdouble volume_0_to_1)
{
  WasapiData *d = self->backend_data;
  if (d && d->volume)
    ISimpleAudioVolume_SetMasterVolume (d->volume,
                                        (float) CLAMP (volume_0_to_1, 0.0, 1.0),
                                        NULL);
}

static void
wasapi_drain (SpotifyAudioOutput *self)
{
  WasapiData *d = self->backend_data;
  if (!d || !d->client)
    return;

  /* Let what is already queued play out, bounded so a wedged endpoint cannot
   * hang shutdown. */
  for (int i = 0; i < 200; i++) {
    UINT32 padding = 0;
    if (FAILED (IAudioClient_GetCurrentPadding (d->client, &padding)) || padding == 0)
      break;
    Sleep (10);
  }

  IAudioClient_Stop (d->client);
  d->started = FALSE;
}

static void
wasapi_close (SpotifyAudioOutput *self)
{
  wasapi_release (self->backend_data);
  self->backend_data = NULL;
}

static const AudioBackendVtable wasapi_vtable = {
  .open       = NULL,             /* opened by output_wasapi_try_open */
  .write      = wasapi_write,
  .set_volume = wasapi_set_volume,
  .drain      = wasapi_drain,
  .close      = wasapi_close,
};

gboolean
output_wasapi_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{
  WasapiData *d = g_new0 (WasapiData, 1);
  d->channels = channels;

  /* Multithreaded apartment: this runs on the audio worker, never the UI
   * thread. RPC_E_CHANGED_MODE means the thread already joined an apartment,
   * which is still usable — but then we must not CoUninitialize it on the way
   * out, hence tracking whether we were the ones who initialised it. */
  HRESULT hr = CoInitializeEx (NULL, COINIT_MULTITHREADED);
  if (hr == RPC_E_CHANGED_MODE) {
    d->com_initialised = FALSE;
  } else if (SUCCEEDED (hr)) {
    d->com_initialised = TRUE;
  } else {
    g_message ("WASAPI unavailable: CoInitializeEx failed (0x%lx)", (unsigned long) hr);
    g_free (d);
    return FALSE;
  }

  if (FAILED (CoCreateInstance (&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **) &d->enumerator))) {
    g_message ("WASAPI unavailable: no device enumerator");
    wasapi_release (d);
    return FALSE;
  }

  if (FAILED (IMMDeviceEnumerator_GetDefaultAudioEndpoint (d->enumerator, eRender,
                                                           eConsole, &d->device))) {
    g_message ("WASAPI unavailable: no default render endpoint");
    wasapi_release (d);
    return FALSE;
  }

  if (FAILED (IMMDevice_Activate (d->device, &IID_IAudioClient, CLSCTX_ALL,
                                  NULL, (void **) &d->client))) {
    g_message ("WASAPI unavailable: could not activate audio client");
    wasapi_release (d);
    return FALSE;
  }

  /* Plain interleaved 16-bit PCM — exactly what every other backend is fed, so
   * the engine needs no Windows-specific conversion. */
  WAVEFORMATEX fmt = { 0 };
  fmt.wFormatTag      = WAVE_FORMAT_PCM;
  fmt.nChannels       = (WORD) channels;
  fmt.nSamplesPerSec  = (DWORD) rate;
  fmt.wBitsPerSample  = 16;
  fmt.nBlockAlign     = (WORD) (channels * 2);
  fmt.nAvgBytesPerSec = (DWORD) rate * fmt.nBlockAlign;
  fmt.cbSize          = 0;

  DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERT_PCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

  hr = IAudioClient_Initialize (d->client, AUDCLNT_SHAREMODE_SHARED, flags,
                                WASAPI_BUFFER_100NS, 0, &fmt, NULL);
  if (FAILED (hr)) {
    /* Retry without the converter flags: a few endpoint drivers reject them,
     * in which case an exact format match may still succeed. */
    hr = IAudioClient_Initialize (d->client, AUDCLNT_SHAREMODE_SHARED, 0,
                                  WASAPI_BUFFER_100NS, 0, &fmt, NULL);
  }
  if (FAILED (hr)) {
    g_message ("WASAPI unavailable: Initialize failed for %d Hz / %d ch (0x%lx)",
               rate, channels, (unsigned long) hr);
    wasapi_release (d);
    return FALSE;
  }

  if (FAILED (IAudioClient_GetBufferSize (d->client, &d->buffer_frames)) ||
      d->buffer_frames == 0) {
    wasapi_release (d);
    return FALSE;
  }

  if (FAILED (IAudioClient_GetService (d->client, &IID_IAudioRenderClient,
                                       (void **) &d->render))) {
    wasapi_release (d);
    return FALSE;
  }

  /* Per-stream volume is optional: the engine already applies volume in
   * software, so an endpoint that will not hand over this interface is fine. */
  if (FAILED (IAudioClient_GetService (d->client, &IID_ISimpleAudioVolume,
                                       (void **) &d->volume)))
    d->volume = NULL;

  if (FAILED (IAudioClient_Start (d->client))) {
    wasapi_release (d);
    return FALSE;
  }
  d->started = TRUE;

  self->vtable       = &wasapi_vtable;
  self->backend_data = d;

  g_message ("WASAPI: shared-mode stream at %d Hz, %d ch, %u-frame buffer",
             rate, channels, (unsigned) d->buffer_frames);
  return TRUE;
}

#else /* !G_OS_WIN32 */

/* Kept compilable everywhere so output.c needs no conditional at the call
 * site; it only ever reaches this on Windows anyway. */
gboolean
output_wasapi_try_open (SpotifyAudioOutput *self, gint rate, gint channels)
{
  (void) self; (void) rate; (void) channels;
  return FALSE;
}

#endif /* G_OS_WIN32 */
