# Deleting playlists, liking albums, following artists

Read-only research. **No write has been attempted for any of these**, on any
account — see the note at the bottom.

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

So:

| action | call |
|---|---|
| like an album | set `"album"`, uri `spotify:album:<id>`, `is_removed = FALSE` |
| unlike an album | same, `is_removed = TRUE` |
| follow an artist | set `"artist"`, uri `spotify:artist:<id>` |
| unfollow an artist | same, `is_removed = TRUE` |

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

So `REM = 3` is derived from two anchors rather than assumed — but it is still
inference, and it is the one number here that a live check should confirm
before it is trusted.

### What a REM needs

Unlike the collection, playlist changes are revision-checked and
position-based: `playlist.c` already reads the rootlist head for its revision
before posting. A `Rem` additionally needs the item's **index** in the
rootlist, so the flow is read the rootlist, find the entry, then post the
removal against that revision and index. Getting the index wrong removes the
wrong playlist, which is why this one deserves the throwaway account and a
list with nothing in it worth keeping.

## Why none of this has been run

The account that lost 4,806 liked songs lost them to a *collection write*
issued while probing — see [[liked-songs-incident]]. Two of the three
operations above are collection writes and the third removes playlists.

The logs currently show two different usernames across builds
(`315kroxz…` and `qaeqggik…`), and nothing on disk says which is the
throwaway. **Confirm the signed-in account before the first write of any of
these.**
