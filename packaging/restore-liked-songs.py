#!/usr/bin/env python3
"""
Restore Liked Songs via PUT /v1/me/tracks with timestamped_ids.

Preserves the original added_at for every track (ISO 8601, from the pre-damage
snapshot). 50 ids per request, oldest first so ordering is right even if a run
is interrupted. Honours Retry-After on 429 rather than hammering, and records
progress so a re-run resumes instead of starting over.
"""
import json, os, sys, time, datetime, urllib.request, urllib.error

TSV      = "~/SpotifyGTK/.local-backups/liked-songs-4806.tsv"
PROGRESS = "~/SpotifyGTK/.local-backups/restore-progress.txt"
TOKEN    = os.path.expanduser("~/.config/spotify-native/token")
BATCH    = 50

def token():
    return open(TOKEN).read().split("\n")[0].strip()

def load():
    rows = [l.rstrip("\n").split("\t") for l in open(TSV)][1:]
    out = []
    for uri, ts in rows:
        if uri.startswith("spotify:track:"):
            out.append((uri.split(":")[-1], int(ts)))
    out.sort(key=lambda r: r[1])          # oldest first
    return out

def put(batch):
    body = json.dumps({"timestamped_ids": [
        {"id": tid,
         "added_at": datetime.datetime.fromtimestamp(ts, datetime.UTC)
                        .strftime("%Y-%m-%dT%H:%M:%SZ")}
        for tid, ts in batch]}).encode()
    req = urllib.request.Request(
        "https://api.spotify.com/v1/me/tracks", data=body, method="PUT",
        headers={"Authorization": "Bearer " + token(),
                 "Content-Type": "application/json"})
    return urllib.request.urlopen(req).status

def main():
    tracks = load()
    done = 0
    if os.path.exists(PROGRESS):
        try: done = int(open(PROGRESS).read().strip())
        except ValueError: done = 0

    print(f"{len(tracks)} tracks total, resuming at {done}", flush=True)

    i = done
    consecutive_429 = 0
    while i < len(tracks):
        batch = tracks[i:i + BATCH]
        try:
            st = put(batch)
            i += len(batch)
            consecutive_429 = 0
            open(PROGRESS, "w").write(str(i))
            if (i // BATCH) % 5 == 0 or i >= len(tracks):
                print(f"  {i}/{len(tracks)}  (HTTP {st})", flush=True)
            time.sleep(0.4)
        except urllib.error.HTTPError as e:
            if e.code == 429:
                # Retry-After is a floor, not a target. Thirteen retries at
                # exactly the advertised 60s made no progress at all, and
                # tight retries plausibly keep the window alive -- so back off
                # harder the longer it persists. Waiting too long is free;
                # waiting too little demonstrably is not.
                consecutive_429 += 1
                wait = int(e.headers.get("Retry-After", "10")) + 2
                wait = max(wait, min(600, 15 * (2 ** min(consecutive_429 - 1, 5))))
                print(f"  429 #{consecutive_429} at {i}; sleeping {wait}s", flush=True)
                time.sleep(wait)
                continue
            elif e.code in (401, 403):
                print(f"  HTTP {e.code} at {i}: {e.read().decode()[:200]}", flush=True)
                print("  token lacks scope or expired -- stopping", flush=True)
                return 1
            else:
                print(f"  HTTP {e.code} at {i}: {e.read().decode()[:200]}", flush=True)
                time.sleep(5)
        except Exception as ex:
            print(f"  {type(ex).__name__} at {i}: {ex}", flush=True)
            time.sleep(5)

    print(f"DONE: {i}/{len(tracks)} restored", flush=True)
    return 0

sys.exit(main())
