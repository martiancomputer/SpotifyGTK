# Deleting playlists, liking albums, following artists

All three are **proven live** against the throwaway account (`315kroxz…`):
each written, read back present, removed, read back absent.

## Liking an album, following an artist

Both are the collection write this client already performs for Liked Songs.
The only thing that changes is the *set*.

```c
spotifygtk_collection_v2_write (mercury, username, set, uris, n, is_removed, cb, ud);
```

`collection.h` defines one set today:

```c
#define SPOTIFYGTK_COLLECTION_SET_LIKED "collection"
```

The shipped binary's string table carries the full list of set names as
standalone strings — `collection`, `album`, `artist`, `episode`, `show` — and
the matching decoration types confirm they are real collections rather than
incidental words:

```
spotify.collection_cosmos.proto.CollectionAlbum
spotify.collection_cosmos.proto.CollectionArtist
spotify.collection_cosmos.proto.CollectionEpisode
spotify.collection_cosmos.proto.CollectionShow
spotify.collection_cosmos.proto.CollectionAddRemoveItem
```

### The obvious set name is wrong

`"album"` is in the string table next to `"artist"` and looks like the answer.
It is not: reading *or* writing that set answers **403**. Probing every
candidate read-only:

| set | result |
|---|---|
| `collection` | 200 — liked tracks |
| `artist` | 200 — followed artists |
| `show` | 200 |
| `album`, `albums`, `artists`, `episode`, `your_library` | **403** |

Saved albums live in **`collection`**, the same set as liked songs,
distinguished only by the URI. Writing `spotify:album:<id>` to `collection`
returned 200 and the album came back as the first item on the next read.

| action | set | uri |
|---|---|---|
| like an album | `collection` | `spotify:album:<id>` |
| follow an artist | `artist` | `spotify:artist:<id>` |
| the reverse of either | same | `is_removed = TRUE` |

The write encoder sends the URI as a string and lets the server infer the
type, so nothing else changes between the two.

**Consequence worth knowing:** because albums share the liked-songs set,
anything reading `collection` gets album URIs mixed in with track URIs.
`window.c` accumulates that set into `liked_uris` with no type filter. It is
harmless today -- the Liked Songs page is built from a context resolve rather
than from that set, and an album URI never matches a track URI when marking
hearts -- but a "saved album" indicator would want to read it deliberately
rather than by accident.

Reading them back is `spotifygtk_collection_v2_read_page()` with the same set,
which is how the library's saved albums and followed artists would be listed.

This is the additive v2 endpoint, not the legacy one — see
[[collection-native-path]] and the header comment in `collection.h`. The legacy
`hm://collection/collection/<user>` replaces the whole set on every write and
is what destroyed a library once already.

## Deleting a playlist

There is no delete. A playlist is *unfollowed*, which removes it from the
user's rootlist and leaves the playlist itself alone — the same operation for
a playlist you made and one you saved. The binary names the concept directly:

```
spotify.playlist4.proto.UnfollowedListItem
CAN_BE_UNFOLLOWED
```

The endpoint is the one `playlist.c` already posts to when it files a new
playlist *into* the rootlist:

```
POST hm://playlist/v2/user/<user>/rootlist/changes
```

with an `Op` whose kind is `REM` instead of `ADD`.

### The Op.Kind numbering

Recovered from the descriptor:

```
KIND_UNKNOWN, ADD, REM, MOV, UPDATE_ITEM_ATTRIBUTES,
UPDATE_LIST_ATTRIBUTES, UPDATE_ITEM_URIS
```

The values are not consecutive from zero, and two are already known here:
`playlist.c` uses `ADD = 2` (proven live), and its header records
`UPDATE_LIST_ATTRIBUTES = 6`. Those two fixed points force the rest:

| name | value |
|---|---|
| KIND_UNKNOWN | 0 |
| ADD | **2** (proven) |
| REM | **3** |
| MOV | 4 |
| UPDATE_ITEM_ATTRIBUTES | 5 |
| UPDATE_LIST_ATTRIBUTES | **6** (documented) |
| UPDATE_ITEM_URIS | 7 |

`REM = 3` was derived from those two anchors and has since been confirmed by
running it.

### What a REM needs

Unlike the collection, playlist changes are revision-checked and
position-based: `playlist.c` already reads the rootlist head for its revision
before posting. A `Rem` additionally needs the item's **index** in the
rootlist, so the flow is read the rootlist, find the entry, then post the
removal against that revision and index.

Getting the index wrong removes a different playlist, so
`spotifygtk_playlist_remove()` refuses rather than guesses: no URI match in the
rootlist means it does nothing and reports failure.

Run end to end on the throwaway, creating something disposable first so that a
wrong index would have been visible against a known list:

```
[before]        3 entries
create          -> spotify:playlist:0DVN16wAz7x0ufl9EbMfI1
[after-create]  4 entries, the new one at index 3
unfollowing spotify:playlist:0DVN…  at rootlist index 3 of 4   -> 200
[after-remove]  3 entries, identical URIs, identical order
```

The last line is the point: the removal took the intended entry and moved
nothing else.

## Accounts

`qaeqggik…` is the main account. `315kroxz…` is the throwaway, and is what
every write above was run against. Worth noting that the engine harness
authenticates from the same stored token as the GUI, so it follows whichever
account is signed in -- it is not pinned to one.

Every write recorded here was run against `315kroxz…` and reversed afterwards,
so the account is back where it started.
