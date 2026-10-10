# Install SpotifyGTK on Windows

Requires Windows 10 version 2004 or newer, or Windows 11, on x64 hardware.
Spotify streaming requires a Premium account.

This build is self-signed by `CN=SpotifyGTK`, not signed by Spotify or a public
certificate authority. Only trust the included certificate if you trust the
source of this package. No private signing key is distributed.

1. Keep `SpotifyGTK.cer` beside the `.msix` file.
2. Double-click `SpotifyGTK.cer`, choose **Install Certificate**, then
   **Local Machine**. Approve the administrator prompt.
3. Choose **Place all certificates in the following store**, browse to
   **Trusted People**, and finish. Do not select Trusted Root Certification
   Authorities.
4. Double-click the `.msix` and choose **Install**. Launch SpotifyGTK from Start.

Alternatively, in an administrator PowerShell window opened in this directory:

```powershell
Import-Certificate -FilePath .\SpotifyGTK.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
```

Then, in an ordinary PowerShell window for the user who will run the app:

```powershell
$package = Get-ChildItem .\SpotifyGTK_*_x64.msix | Select-Object -First 1
Add-AppxPackage -Path $package.FullName
```

The package bundles its GTK runtime, icon themes, fonts, image loaders,
GSettings schemas, and HTTPS certificate database. MSYS2, GTK, and separate
font installations are not required. Fonts are private to the app; it does
not install fonts system-wide.

The `.cer` contains only the public certificate. Verify the downloaded files
against `SHA256SUMS.txt` with `Get-FileHash -Algorithm SHA256` if needed.

To uninstall, use Windows Settings → Apps → Installed apps → SpotifyGTK.
If no other builds use the certificate, it can also be removed from
Local Computer → Trusted People → Certificates in `certlm.msc`.
