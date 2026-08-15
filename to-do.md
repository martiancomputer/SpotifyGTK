# To-do

Reported from real use on 2026-08-14, against `23f797f`. Linux unless a note
says otherwise; the Windows build was brought up the same day and is running.

Lines beginning **note:** are added context — commit references, suspicions,
or a question that needs answering before the item can be picked up. Everything
else is as reported.

---

## Bugs

- [~] **Scrolling randomly jumps the viewport to the top or bottom.** The smooth
      scroller clamped its target once, at wheel time, against the bounds as
      they were then — and these lists grow and shrink under the pointer. When
      the content shrank mid-flick the animation kept driving past the new end,
      GTK clamped every step, and the view slid to one end and stayed. Now
      re-clamped each tick, stopping where the list can actually go.
      *note:* plausible cause matched to the symptom, not a reproduction. If it
      still happens, say so — it would mean a second cause.

- [x] **The playing-row indicator sticks on a track that has finished.** Seen on
      Liked Songs: the equaliser stayed on "Unhappy Woman (Remix)" while
      "Oligarch" played, with the playback bar and Now Playing panel both
      correct. `on_now_playing_changed()` updated `current_track_uri`, the
      progress bar and the panel, but never told the lists — so the indicator
      only moved when something *else* refreshed it, either a player state
      change or a page navigation. A gapless handover is neither: the sounding
      track changes without the state changing. One `broadcast_playing_uri()`
      now serves all three moments, including the handover that was missing.

- [ ] **`GLib-CRITICAL: Source ID N was not found when attempting to remove it`,
      occasionally at shutdown.** Seen twice in ~20 runs, never reproducibly.
      *note:* **audited, and the obvious cause is not there.** All 19
      `g_timeout_add` registrations and all 21 `g_source_remove` calls were
      checked: every callback that returns `G_SOURCE_REMOVE` clears its own id
      first, every removal is either guarded by the id being non-zero or uses
      `g_clear_handle_id`, and the `g_source_destroy` sites work on pointers
      rather than ids. `on_collection_changed` — the one that removes and
      re-arms on every collection event, and so the likeliest — guards,
      removes and reassigns correctly.
      So it is not the "forgot to zero the id" class, which is what it looks
      like. What is left is a race (a source firing as it is being removed) or
      a source attached to a context other than the default, which
      `g_source_remove` does not search — `main.c` already notes that trap for
      the engine's private context. Needs a reproduction, and it is harmless
      enough that it is not worth guessing at.

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

- [x] **Liking a song sometimes reloads the whole Liked Songs page.** The grace
      window that recognises our own echo was stamped when the write was
      *issued*, but the server emits the change only once it has applied it —
      so a slow write spent its grace waiting and the echo landed after it
      expired, costing a full collection re-read. Re-stamped when the write
      lands. That is also what "sometimes" was: how long the write took.

- [x] **Unsaving an album sometimes does nothing visually**, though the write
      does reach the server and the album is removed there. Same mechanism seen
      from the other side: the button flipped, but the Library grid only ever
      learned of a removal from the next full collection read — the very thing
      the grace window suppresses. So whether it appeared to work came down to
      whether the echo landed outside the window. The card is now removed
      locally, which is what should have happened regardless.

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

- [!] **Discord activity.** Blocked on a decision, and the answer to "how does
      the official client do it" is that **it does not**. Discord's "Listening
      to Spotify" card comes from the account link in Discord's own settings —
      Discord reads the playback state from Spotify server-side, which is why it
      carries album art, a live progress bar and a "Play on Spotify" button that
      other people can click. None of that is reachable by a local client, and
      no third-party app can produce that card.
      What *is* reachable is ordinary Rich Presence over the local IPC socket,
      which shows as a generic activity under our own name.
      *note:* **measured here: there is no `discord-ipc-*` socket on this
      machine and no Discord desktop client installed** — Discord is being used
      in a browser, and a browser tab cannot receive Rich Presence. So this
      would do nothing as things stand. It needs the Discord desktop app, plus
      an application id registered on Discord's developer portal to appear
      under a name at all. Both are yours to decide before any code is worth
      writing. (The account-link explanation above is background knowledge, not
      something probed here.)

- [~] **"Remove from this Playlist" in the track context menu**, shown when
      viewing a playlist. Dynamic: replaces "Add to playlist" when the track is
      already in the playlist being viewed.
      **Works through the UI**, confirmed in use. Menu entry swaps on a playlist page, the row position travels with
      it, and `spotifygtk_playlist_remove_track()` reconciles that row against
      what the server returns — taking the named row if it holds the track, a
      single match if there is only one, and refusing otherwise rather than
      removing the wrong copy of a duplicated track.
      **Verified live** on the throwaway (`315kroxz…`, confirmed by reading the
      logged-in username before writing anything). A disposable playlist was
      created holding A B A C — the duplicate deliberate, since that is the
      case the code exists to get right — row 2 was removed, and the read-back
      gave A B C: the *named* row went, the other copy of A survived at index 0
      and its neighbours are untouched. Both probe playlists were unfollowed
      afterwards, so nothing was left behind.
      *note:* the probe had to run inside the GUI rather than the harness. The
      throwaway is not Premium, so the harness's audio-key step fails and quits
      its loop before an async chain of writes can finish.

- [ ] **Find out how Spotify does Smart Shuffle.**

- [x] **Context menu on pinned sidebar items**, with "Unpin". Done — right-click
      a pinned row. Not dynamic, as asked: the row exists because the thing is
      pinned, so the only verb is the one that undoes it.

- [x] **Use less Spotify green.** Green is now genuinely state only — liked,
      followed, pinned, active toggle, selected sidebar row. The two largest
      green surfaces were carrying no meaning and are neutral now: the progress
      fill (position is read from where the fill ends, not its colour) and the
      equaliser bars. Say if it has gone too far the other way.

- [~] **Rework the playlist creation wizard** to fit the rest of the client,
      and make it non-movable. Non-movable done: the AdwHeaderBar it carried
      *was* the drag handle, so it is gone and the title now sits in the
      dialog's own content; also fixed-size, with Escape to close.
      Made non-modal: a modal grab covers the whole parent surface, and with
      client-side decorations the title bar is part of that surface — so the
      app could not be moved, resized or closed while a dialog was open, and
      GTK has no way to exempt a region. Transient alone keeps it above its
      parent, plus destroy-with-parent now that closing the window underneath
      is possible.
      Buttons rescored: the primary action in a dialog is white now, not the
      accent pill. login-button is right for signing in — one green thing on an
      empty screen — but reused in dialogs it was the loudest colour in the
      window, for actions carrying no state at all. Cancel added to both
      dialogs and Escape bound: with the title bar gone there was no close
      button, and a dialog you can only leave by completing it is a trap.
      Nothing is written on the way out, so dismissing discards.
      Reworked further: the rows showed the raw `spotify:playlist:37i9…` URI
      until each name lookup returned, which is most of why it read as debug
      output — a quiet "Loading…" now, going to the name and from dim to normal
      text when it arrives. Sections named ("New playlist" / "Your playlists"),
      rows padded with a hover, long names ellipsised.
      *note:* the fuller answer to "fit in with the client" is AdwDialog, a
      sheet drawn inside the window rather than a separate one. That needs
      libadwaita 1.5 and the floor here is 1.4 (Ubuntu 24.04 ships 1.5.0, so in
      practice nothing would be lost) — your call whether to raise it.

- [~] **Edit playlist name and artwork.** Name done: right-click a playlist card
      → "Rename…", shown only for playlist URIs since an album's name is the
      release's, not the user's. **Verified live** on the throwaway — created a
      disposable playlist, renamed it, read the name back from the playlist
      itself and got the new one; unfollowed afterwards.
      *note:* the rename needed nothing new on the wire. Creating a playlist
      was always create-then-rename, so the UPDATE_LIST_ATTRIBUTES op was
      already proven; this only exposes it.
      *note:* then it crashed on click, caught by ASan. album_menu_emit_and_close
      sent three arguments for the pin and one for everything else, and the new
      signal was declared with three — so emitting it read one argument off the
      varargs and two off the stack, and the marshaller called g_strdup on
      them. It now always sends all three: g_signal_emit reads exactly as many
      as the signal declares, so too many is harmless and too few is not.
      *note:* the menu entry did nothing at first. The playlists grid is wired
      by hand — it activates into a playlist rather than an album — so it never
      got the rename or pin handlers, while the menu offered them anyway. The
      shared part is one function now, called from both.
      *note:* **artwork is not done.** It is an image upload rather than a
      playlist4 op, so it is a different endpoint and a separate piece of work.

- [~] **Profile picture and username in Settings**, under a new `# User`
      heading, above the logout button. The heading and the account are in,
      above the log-out row — which is the one place it matters, since this
      client can hold either of two accounts and the button gave no clue which
      it was about to forget. Verified: the label reads the signed-in id.
      *note:* **the picture is not done, and it is not a small addition.** The
      client shows the Spotify user id because that is what it has. A display
      name and an avatar live behind the Web API, which nothing in this codebase
      speaks — no bearer token is exposed and `api.spotify.com` appears nowhere
      — so it needs an endpoint found and proven first, the same way everything
      else here was.

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

- [ ] **Six crashes on 2026-08-15** — 10:27, 15:22, 18:27, 21:33, 22:31, 23:09,
      alternating SIGABRT and SIGSEGV. No frame of ours near the top of any of
      them. The clearest (21:33) is `on_header_read` emitting a signal and
      `g_closure_invoke` jumping to `0x7f860000012e` — a garbage function
      pointer, which is a damaged closure rather than a dangling user_data (that
      would fault *inside* a handler, not on the way to one). Consistent with
      one corruption bug underneath all six.
      *note:* **the release build cannot be trusted to unwind.** gdb produced
      `spotifygtk_album_grid_set_content_margins (self=0x7, start=22001, …)` on
      several threads at once — stack scanning without frame pointers, and it
      would have been reported as the culprit if taken at face value. Use
      systemd's own trace (`coredumpctl info`), or run `build-prof/`.

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
