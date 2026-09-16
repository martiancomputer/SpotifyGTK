# Release packages

The release workflow stages each tag's generated packages under
`releases/<tag>/` during the publish job and attaches the same files to the
GitHub Release. These are build outputs, not source-controlled binaries: the
workflow deliberately does not commit them back to `main`, so the repository
does not grow by the size of every `.deb`, AppImage, and MSIX.

For a published tag, use the GitHub Release assets or reproduce the local
staging layout with the workflow's downloaded artifacts.
