$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Windows = Join-Path $Root 'apps/spotify-native-windows'
$Dist = Join-Path $Windows 'build/dist'
$Out = Join-Path $Root 'packaging/out'
New-Item -ItemType Directory -Force $Out | Out-Null
$manifest = Join-Path $Dist 'AppxManifest.xml'
@"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10" xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10">
  <Identity Name="com.github.spotifygtk.SpotifyNative" Publisher="CN=SpotifyGTK" Version="0.1.0.0" />
  <Properties><DisplayName>SpotifyGTK</DisplayName><PublisherDisplayName>SpotifyGTK</PublisherDisplayName><Description>Native Spotify client</Description><Logo>Assets/logo.svg</Logo></Properties>
  <Resources><Resource Language="en-us" /></Resources>
  <Dependencies><TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0" /></Dependencies>
  <Applications><Application Id="SpotifyNative" Executable="spotify-native.exe" EntryPoint="Windows.FullTrustApplication"><uap:VisualElements AppListEntry="none" DisplayName="SpotifyGTK" Description="Native Spotify client" /></Application></Applications>
</Package>
"@ | Set-Content -Encoding UTF8 $manifest
New-Item -ItemType Directory -Force (Join-Path $Dist 'Assets') | Out-Null
Copy-Item (Join-Path $Root 'apps/spotify-native/data/com.github.spotifygtk.SpotifyNative.svg') (Join-Path $Dist 'Assets/logo.svg') -Force
$MakeAppx = Get-Command makeappx.exe -ErrorAction SilentlyContinue
if (-not $MakeAppx) { throw 'makeappx.exe is required (install the Windows SDK)' }
$Msix = Join-Path $Out 'SpotifyGTK-0.1.0-x64.msix'
& $MakeAppx pack /d $Dist /p $Msix /o
Write-Host "Created $Msix"
