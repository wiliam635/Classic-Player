# Public update feed

The desktop app and VST3/AU editors check the HTTPS feed ten seconds after opening,
then every six hours while open. The Updates button also checks manually. Requests
run on a background thread, use a five-second connection timeout and accept at most
64 KiB of JSON. They send no account, license, or machine information.

Default feed: `https://licenca.classickeys.com.br/updates/classic-player.json`.
Override at build time with `-DCLASSIC_PLAYER_UPDATE_FEED_URL=https://...`.
The feed must be publicly accessible without GitHub credentials. The project
repository is private; GitHub Actions artifact links expire and must not be used
as customer download links.

Publish `classic-player.json` on the official HTTPS site. Fill the `downloads`
fields with permanent installer URLs and `releaseNotesUrl` with an optional page
describing the release. Update `version` only when the corresponding installers
are ready for users. Stable versions use three numbers, e.g. `2.0.3`. Prerelease
versions and incomplete download URLs never trigger an update notification.

The shipped example describes the installed version and points both platforms
to `https://licenca.classickeys.com.br/membros`, the existing members area.
It does not announce a newer release. Publishing the feed and recording the
new stable version is required to activate notifications for customers.

When a higher stable version exists for the current platform, the Updates button
shows UPDATE AVAILABLE. Clicking it opens details, Download Update, Later, and an
optional Release Notes link. Download opens the system browser. No installation
is attempted while the audio engine or a DAW is running.

The CPU meter displays the processing time used by Classic Player relative to
the time available for each audio buffer. It covers this processor and its hosted
instruments, not the computer's total CPU usage. The audio callback uses JUCE's
AudioProcessLoadMeasurer; the UI reads its smoothed result and repaints only when
the displayed percentage changes.
