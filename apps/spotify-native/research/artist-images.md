# Artist images

Two different images, and the client was showing the wrong one for as long as
the artist page has existed.

| id prefix | what it is | measured |
|---|---|---|
| `ab676161…` | round avatar | 269×496 → 269×320, 0.84:1 |
| `ab676170…` | backdrop of the pinned "Artist pick" card — a promo photo | 660×496, 1.33:1 |
| `ab67616d…` | album cover | — |

The prefix is the only reliable way to tell them apart. Both are plain
`i.scdn.co/image/<40 hex>` ids and nothing else in the payload marks which is
which.

## The GraphQL route does not work

`api-partner.spotify.com/pathfinder/v1/query`, operation `queryArtistOverview`,
persisted-query hash `1ac33dda…737c72` — all read correctly out of the shipped
client's bundle — answers:

```json
{ "error": { "status": 403, "message": "Client/request not allowed" } }
```

for every artist. Pathfinder does not accept our client token; that endpoint
belongs to the web player, whose token is minted against a different client id.
Sending `Authorization`, `Accept` and `Client-Token` is not enough.

**This failed silently for months.** A missing header is legitimately not an
error — plenty of artists have not uploaded one — so the code fell back to the
avatar, an image appeared, and the page looked like it worked. The fallback
only logged when it had a `GError`, and a 403 body parsed fine into "no image
found", so there was never a `GError` to log. Everything downstream was then
debugged as a *sizing* problem, because a 0.84:1 avatar stretched across a
landscape panel does look like one.

The lesson is the logging, not the endpoint: a fallback that fires on the
normal path and the broken path alike, and says nothing on either, cannot be
told apart from success.

## The real header is out of reach

**The banner the desktop client draws is not obtainable.** Confirmed, not assumed:

- **pathfinder is the only source.** `artistUnion.visuals.headerImage`. The
  operation name and hash in `322.js` match what we send exactly.
- **It refuses us.** 403 for bearer alone, bearer + client-token, and with
  `app-platform` and `spotify-app-version` added. Both tokens are present and
  valid (438 and 388 bytes). Our client id is keymaster,
  `65b708073fc0480ea92a077233ca87bd`, which is not entitled to that endpoint.
- **The native protocol does not carry it.** The full `ExtensionKind` enum —
  `UNKNOWN_EXTENSION, CANVAZ, STORYLINES, PODCAST_TOPICS, PODCAST_SEGMENTS,
  AUDIO_FILES, TRACK_DESCRIPTOR, PODCAST_COUNTER, ARTIST_V4=8, ALBUM_V4=9,
  TRACK_V4=10, …` — has no visuals kind. The binary contains no `hm://` artist
  endpoint and no spclient path matching image/visual/gallery/header.
- **The public web page has nothing to scrape.** `open.spotify.com/artist/<id>`
  and its `/embed/` variant are client-rendered shells: zero `i.scdn.co` ids,
  zero `headerImage`. `get_access_token` is blocked outright.

So the choice is between a promo photo, the avatar, or no image — not between
those and the real banner.

## What is reachable: `hm://artistview/v1/artist/<id>?format=json`

Straight over the AP connection we already hold. 64 KB of JSON, the same
payload the official artist screen is built from:

```
{ id, title, header, body[], custom }
```

This is the **mobile** artist page (`ubi:specification_id: mobile-artist-page`),
which is why there is no banner in it — that design uses a circular avatar, and
`header.images.main` says so outright with `"custom": {"style": "circular"}`.

The one landscape image hangs off whichever body section carries it:

```
body[7]  id=pinned_item_row  component=artist:pinnedItemV2
         images.background.uri → https://i.scdn.co/image/ab6761700000c52c…
```

That is the **backdrop of the "Artist pick" card** — verified by looking at the
image, not by inferring from its aspect ratio, which is the mistake that put an
avatar on the page in the first place. It is a promo photo of the artist: real,
landscape, and *not* the header. Artists with no pinned pick have none at all,
and then the page falls back to the avatar.

Placement is not dependable, so the scan looks for a 40-hex id starting
`ab676170` anywhere in the payload rather than walking a fixed path.

`hm://artist/v1/<id>/desktop?format=json` — the librespot-era endpoint — comes
back empty and is gone.

### Only one size exists

`c52c` is the only variant that resolves. `e5eb`, `b273`, `1e02`, `5174`,
`10ca`, `ecd8`, `f178`, `bd5e` all 404. So 660×496 is the whole of it, and the
banner necessarily upscales.

## Sizing

`gtk_widget_set_size_request()` sets a *minimum*. `GtkPicture` reports its
texture's size as its natural size, so a box holding one grows to the image —
asking for 320 and measuring 511. Inside a scrolled window, which hands out
natural height, nothing pushes back.

The hero is therefore a `GtkOverlay`: an overlay child that is not a
measure-overlay contributes nothing to measurement and is allocated the
overlay's size. An empty sizer box carries the height, the picture fills what
it is given, and `GTK_OVERFLOW_HIDDEN` clips the cover overflow to the rounded
corners.

Verified at 1169 px wide: texture 1024×770, allocation 1169×260.
