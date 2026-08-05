#!/usr/bin/env python3
"""
Restore Liked Songs via PUT /v1/me/tracks with timestamped_ids.

Preserves the original added_at for every track (ISO 8601, from the pre-damage
snapshot). 50 ids per request, oldest first so ordering is right even if a run
is interrupted. Honours Retry-After on 429 rather than hammering, and records
progress so a re-run resumes instead of starting over.
"""
import json, os, sys, time, datetime, subprocess, urllib.request, urllib.error, urllib.parse

TSV      = "~/SpotifyGTK/.local-backups/liked-songs-4806.tsv"
PROGRESS = "~/SpotifyGTK/.local-backups/restore-progress.txt"
TOKEN    = os.path.expanduser("~/.config/spotifygtk/tokens")
BATCH    = 50
CLIENT_ID = "<set SPOTIFY_CLIENT_ID>"

def keyring_blob():
    try:
        out = subprocess.run(["secret-tool", "lookup", "type", "tokens"],
                             capture_output=True, text=True, timeout=10)
        return out.stdout if out.returncode == 0 else ""
    except Exception:
        return ""


def token():
    """
    The connect-flow token, from the keyring first and the config file second --
    the same order auth.c writes them in.

    Deliberately NOT the native client's token at ~/.config/spotify-native/token.
    That one comes from Spotify's desktop OAuth flow and api.spotify.com refuses
    it with 429 on the very first request, before any traffic exists to rate
    limit, which reads exactly like throttling and is not.
    """
    try:
        out = subprocess.run(["secret-tool", "lookup", "type", "tokens"],
                             capture_output=True, text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.split("\n")[0].strip()
    except Exception:
        pass
    return open(TOKEN).read().split("\n")[0].strip()

def refresh_token():
    """
    Exchange the stored refresh token for a new access token and write it back.

    Access tokens last an hour, and this restore may sit waiting far longer than
    that for a clamp to lift, so expecting a human to re-run the sign-in at the
    right moment is the wrong design. The refresh grant needs no consent -- the
    user already approved these scopes.
    """
    blob = keyring_blob()
    parts = blob.split("\n") if blob else []
    if len(parts) < 2 or not parts[1].strip():
        return None
    data = urllib.parse.urlencode({
        "grant_type": "refresh_token",
        "refresh_token": parts[1].strip(),
        "client_id": CLIENT_ID}).encode()
    req = urllib.request.Request("https://accounts.spotify.com/api/token",
                                 data=data, method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    try:
        d = json.load(urllib.request.urlopen(req))
    except Exception as e:
        print(f"  token refresh failed: {e}", flush=True)
        return None
    new = d["access_token"]
    keep = d.get("refresh_token", parts[1].strip())
    exp = int(time.time()) + int(d.get("expires_in", 3600))
    subprocess.run(["secret-tool", "store", "--label=SpotifyGTK tokens", "type", "tokens"],
                   input=f"{new}\n{keep}\n{exp}\n", text=True, capture_output=True)
    print("  refreshed access token", flush=True)
    return new


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
            elif e.code == 401:
                if refresh_token():
                    continue
                print("  401 and refresh failed -- sign in again", flush=True)
                return 1
            elif e.code == 403:
                # Not a scope problem: a refreshed token was confirmed to carry
                # user-library-modify and still got 403 here, while the same
                # account's Mercury library writes kept returning 201. It is an
                # account-side clamp on Web API library mutation, and it lifts
                # on its own -- so stop cleanly and let a later run resume.
                print(f"  HTTP 403 at {i}: library writes are still clamped", flush=True)
                return 2
            else:
                print(f"  HTTP {e.code} at {i}: {e.read().decode()[:200]}", flush=True)
                time.sleep(5)
        except Exception as ex:
            print(f"  {type(ex).__name__} at {i}: {ex}", flush=True)
            time.sleep(5)

    print(f"DONE: {i}/{len(tracks)} restored", flush=True)
    return 0

sys.exit(main())
