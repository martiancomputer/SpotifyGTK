# Artist images

Five image classes hang off an artist, and four of them are not the banner.
Telling them apart by id prefix is the only reliable method — nothing in any
payload labels them.

| id prefix | what it is | shape |
|---|---|---|
| `ab676186…` | **the banner** — `artistUnion.headerImage` | 2660×1140, 2.33:1 |
| `ab676161…` | round avatar / profile picture | square, e.g. 640×640 |
| `ab676167…` | gallery — promo photos, used by "About the artist" | square |
| `ab676170…` | backdrop of the pinned "Artist pick" card | 660×496 |
| `ab67616d…` | album cover | square |

The banner's URLs point at `image-cdn-ak.spotifycdn.com` and `i2o.scdn.co`, but
`i.scdn.co` serves the same ids, so the id alone is enough and the cover loader
needs no new host.

## Getting the banner: two things had to be right

It comes from pathfinder, Spotify's GraphQL endpoint — persisted query
`queryArtistOverview`, hash `1ac33dda…737c72`, both read out of the shipped
bundle (`322.js`). Nothing in the native protocol carries it: the `Artist`
descriptor has no header field (see below), the full `ExtensionKind` enum has no
visuals kind, and the binary holds no `hm://` artist route or spclient image
path.

### 1. The User-Agent

`api-partner` answers `403 "Client/request not allowed"` to anything that does
not look like a browser. **That is the entire difference.** Ruled out against a
live server, every one still 403:

- the client id — the shipped client uses **keymaster**, `65b708…87bd`, the same
  one we do. Confirmed in its own `login.spa`. This kills the obvious theory.
- the client token, and `client_version` inside it: `1.2.52.442` (librespot's),
  `1.2.92.147`, `1.2.156.10197`
- `spotify-app-version`, both `1.2.92.147` and the numeric `896000000` the
  bundle actually sends
- `app-platform`: `Linux`, `WebPlayer`, `Win32`
- `origin`/`referer` for `xpui.app.spotify.com` and `open.spotify.com`
- GET and POST

Send a Chrome UA and the request succeeds with the credentials we already had.

**libsoup detail that cost a debugging round:** `SoupSession:user-agent` is
applied to every message the session queues, *overwriting* whatever the message
already set. Putting the browser UA on the `SoupMessage` was silently undone.
Pathfinder therefore gets its own `SoupSession` whose UA is the browser one.

### 2. The field is not under `visuals`

It is **`artistUnion.headerImage`**, top level, an `ImageV2` whose
`data.sources[]` carry `maxWidth`/`maxHeight`.

`visuals` contains `avatarImage` and `gallery` — the round profile picture and
promo photos. Neither is the banner. Reading `visuals.headerImage`, which does
not exist, is why this returned nothing even in the responses that did work.

Ad-hoc GraphQL is refused (`"Missing extensions in the request"`), so the query
cannot be narrowed to the one field wanted; the whole overview comes back.

## How this stayed broken so long

The original code had **both** faults at once, and either alone would have hidden
the other. Worse, a missing banner is legitimately not an error — plenty of
artists have published none — so the code fell back to the avatar, an image
appeared, and the page looked like it worked. The fallback only logged when it
held a `GError`, and a 403 body parses cleanly into "no image found", so there
was never a `GError` to log.

Everything downstream was then debugged as a *sizing* problem, because a 0.84:1
avatar stretched across a landscape panel genuinely does look like one. Two
commits went into geometry before anyone checked what the texture was.

Two lessons, both cheap next time:

- **A fallback that fires identically on the working and broken paths, and is
  silent on both, is indistinguishable from success.** It now logs
  unconditionally.
- **Verify an image by looking at it.** `ab676170…` was accepted as "the header"
  on the strength of its 1.33:1 aspect ratio. Opening the file shows a man in a
  chair — the Artist-pick backdrop. One `Read` would have caught it.

## The native protocol, for the record

`hm://artistview/v1/artist/<id>?format=json` works and returns 64 KB — but it is
the **mobile** artist page (`ubi:specification_id: mobile-artist-page`), whose
design has no banner. Its `header.images.main` is the avatar and says so:
`"custom": {"style": "circular"}`. `hm://artist/v1/<id>/desktop?format=json` is
gone, returning empty.

The `Artist` message descriptor, read out of the binary, in full: `gid, name,
popularity, top_track, album_group, single_group, compilation_group,
appears_on_group, external_id, portrait, biography, activity_period,
restriction, related, is_portrait_album_cover, portrait_group, sale_period,
availability`. No header, no banner, no visuals.

`ExtensionKind`: `UNKNOWN_EXTENSION=0, CANVAZ, STORYLINES, PODCAST_TOPICS,
PODCAST_SEGMENTS, AUDIO_FILES, TRACK_DESCRIPTOR, PODCAST_COUNTER, ARTIST_V4=8,
ALBUM_V4=9, TRACK_V4=10, SHOW_V4, EPISODE_V4, …` — nothing for artist visuals.

## Sizing

`gtk_widget_set_size_request()` sets a *minimum*. `GtkPicture` reports its
texture's size as its natural size, so a box holding one grows to the image —
asking for 320 and measuring 511. Inside a scrolled window, which hands out
natural height, nothing pushes back.

The hero is therefore a `GtkOverlay`: an overlay child that is not a
measure-overlay contributes nothing to measurement and is allocated the
overlay's size. An empty sizer box carries the height, the picture fills what it
is given, and `GTK_OVERFLOW_HIDDEN` clips the cover overflow to the rounded
corners.

Verified at 1125 px wide: texture 1280×549, allocation 1125×300.
