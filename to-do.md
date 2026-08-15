# To-do

Reported from real use on 2026-08-14, against `23f797f`. Linux unless a note
says otherwise; the Windows build was brought up the same day and is running.

Lines beginning **note:** are added context — commit references, suspicions,
or a question that needs answering before the item can be picked up. Everything
else is as reported.

---

## Bugs

- [ ] **Scrolling randomly jumps the viewport to the top or bottom.**

- [ ] **The old scroll problem still returns occasionally.** Rare now, and
      everything scrolls smoothly the rest of the time.
      *note:* the easing landed in `1d38030` (ease by elapsed time, not by
      frame) and `aac56fc`. Rare and intermittent means a measurement problem
      first — needs a reproduction before a fix, or it will be another change
      made on a hunch.

- [ ] **Library and Playlists need the same fix Liked Songs got.**
      *note:* ambiguous, two candidates — `3f118cb` (stop decoding artwork for
      rows the scroll has passed) or `e91626f` (decode card art when a card is
      shown, not when bound). Confirm which symptom is meant before starting.

- [ ] **Liking a song sometimes reloads the whole Liked Songs page.**

- [ ] **Unsaving an album sometimes does nothing visually**, though the write
      does reach the server and the album is removed there.

- [~] **Seeking inside the last 30 seconds drops the track entirely.** Playback
      stops, the song has to be started again, and it does not land on the
      requested position.
      *note:* almost certainly `df850df`, which is mine and landed today. That
      commit made an explicit seek cancel the sounding track's cancellable so
      the sink abandons its queued audio. The abandon clearly works; what
      follows it — restarting the track at the target — evidently does not.
      Before that change the same seek arrived late (the "rubber-band"); now it
      does not arrive. **Treat as a regression, not a new bug**, and consider
      whether reverting is the right first move while it is diagnosed.
      *note (cfeef29):* half of it fixed. seek() returned early when nothing
      was audible, and after the abandon nothing is — so every further drag did
      nothing, which is the "does not go to the intended position" half. A seek
      now goes to the running engine, or is held for the track that is coming.
      Whether the **restart** itself fails is still open; the path now logs
      which branch it took, so one reproduction will say.

- [x] **Remove cover art from pinned sidebar items; text only.** Done. Name and
      type remain; `cover_id` is still stored, so this is presentation only and
      nothing needs re-pinning if it comes back. The backfill that fetched art
      for old pins is gone with it — cover requests at startup went 105 -> 100.
      *note:* the sentence ended mid-thought ("keep them text only, with"), so
      if something was meant to replace the art, say what.

---

## Features

- [ ] **Discord activity.** Work out how the official client does it first.
      *note:* Rich Presence is a local IPC socket rather than anything network
      side, so this is observable from a running official client.

- [ ] **"Remove from this Playlist" in the track context menu**, shown when
      viewing a playlist. Dynamic: replaces "Add to playlist" when the track is
      already in the playlist being viewed.
      *note:* the write is the playlist4 `REM` op — the same one playlist
      deletion uses, documented in `research/library-writes.md`, including that
      it is position-based and refuses rather than guesses.

- [ ] **Find out how Spotify does Smart Shuffle.**

- [x] **Context menu on pinned sidebar items**, with "Unpin". Done — right-click
      a pinned row. Not dynamic, as asked: the row exists because the thing is
      pinned, so the only verb is the one that undoes it.

- [ ] **Use less Spotify green.**

- [ ] **Rework the playlist creation wizard** to fit the rest of the client,
      and make it non-movable.

- [ ] **Edit playlist name and artwork.**

- [ ] **Profile picture and username in Settings**, under a new `# User`
      heading, above the logout button.

---

## Known limitations — not bugs

- **Delay fetching songs is the network, not the client.** On a very low ping
  connection there is no delay at all.

---

## Verified working

- Seek bar, almost always.
- Media previews load very fast.
- Gapless playback.
- Pinning and unpinning.
- Windows: builds, signs in, plays through WASAPI. 169.8 MB in Task Manager
  while playing, against ~122 MB peak heap measured on Linux — consistent with
  the remaining Linux RSS being GPU-side rather than the C heap.

---

## Carried over from earlier today

- [ ] **A use-after-free is still live.** Core from 14:50 on 2026-08-14: GTK
      validating its CSS tree in the frame-clock paint idle, dereferencing an
      unmapped node, with no frame of ours on the stack. `build-asan/` is built
      and clean on every path that can be driven without a person clicking, so
      the next step is a real listening session under it:
      `ASAN_OPTIONS=detect_leaks=0 ./build-asan/src/spotify-native`

- [ ] **`build.sh --bundle` has never been run on Windows.** The fixpoint DLL
      walk is verified logically (`cfb6e1d`) but no bundle has been produced or
      launched on a clean machine. Doing so is what would let someone be handed
      a folder rather than a build guide.
