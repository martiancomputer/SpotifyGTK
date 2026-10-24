# Install SpotifyGTK on Windows

Requires Windows 10 version 2004 or newer, or Windows 11, on x64 hardware.
Spotify streaming requires a Premium account.

This build is self-signed by `CN=SpotifyGTK`, not signed by Spotify or a public
certificate authority. Only trust the included certificate if you trust the
source of this package. No private signing key is distributed.

1. Keep `spotifygtk-1.1.3-release-windows-x86-64.cer` beside the `.msix` file.
2. Double-click the `.cer`, choose **Install Certificate**, then
   **Local Machine**. Approve the administrator prompt.
3. Choose **Place all certificates in the following store**, browse to
   **Trusted People**, and finish. Do not select Trusted Root Certification
   Authorities.
4. Double-click the `.msix` and choose **Install**. Launch SpotifyGTK from Start.

Alternatively, in an administrator PowerShell window opened in this directory:

```powershell
$version = '1.1.3'
Import-Certificate -FilePath ".\spotifygtk-$version-release-windows-x86-64.cer" -CertStoreLocation Cert:\LocalMachine\TrustedPeople
```

Then, in an ordinary PowerShell window for the user who will run the app:

```powershell
$package = Get-ChildItem ".\spotifygtk-$version-release-windows-x86-64.msix" | Select-Object -First 1
Add-AppxPackage -Path $package.FullName
```

The package bundles its GTK runtime, icon themes, fonts, image loaders,
GSettings schemas, and HTTPS certificate database. MSYS2, GTK, and separate
font installations are not required. Fonts are private to the app; it does
not install fonts system-wide.

Enhanced GTK tester packages include **Settings → Performance → Frame rate**:
Default, 30, 60, 120, 144, 240, or 360 FPS. Changes apply immediately and survive
restart. These are experimental rendering ceilings, not changes to your
monitor's refresh rate. Default keeps native GTK pacing; Windows and the
display can limit the actual presented rate.

**Settings → Interface → Interface scale** offers Default, Custom, 25%, 50%,
75%, 90%, 100%, 150%, and 300%. Custom accepts 25–300%. The keybind manager
below it initially exposes scaling up/down and play/pause, defaulting to
Ctrl++, Ctrl+-, and Space. Space remains available for typing in text boxes. F11 toggles
fullscreen; Escape exits fullscreen after dismissing any open dialog.
Side panes hide automatically in narrow snapped windows. Volume, including
mute, is saved in the settings INI and restored on the next launch.

The `.cer` contains only the public certificate. Verify the downloaded files
against `SHA256SUMS.txt` with `Get-FileHash -Algorithm SHA256` if needed.

To uninstall, use Windows Settings → Apps → Installed apps → SpotifyGTK.
If no other builds use the certificate, it can also be removed from
Local Computer → Trusted People → Certificates in `certlm.msc`.
