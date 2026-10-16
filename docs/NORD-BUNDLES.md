# Nord Sound Manager bundle support

Classic Player imports `.ns4pbundle` files into the per-user data folder:

- macOS: `~/Library/Application Support/Classic Player/Nord Banks/`
- Windows: `%APPDATA%/Classic Player/Nord Banks/`

The importer preserves the bundle's relative folders and copies its `meta.xml`,
`.npno`, `.ns4p`, and `.nsmp4` files. It validates the archive before writing,
rejects symbolic links and paths that escape the bank folder, bounds the number
and expanded size of entries, and only exposes a bank to the library after the
copy completes.

At this stage, `.npno` pianos can be selected from the Sample layer's Nord
library selector and are loaded by the existing Nord piano decoder. The layer
state stores the imported file path, so a saved Classic Player state can reload
the selected piano. The original `.ns4pbundle` is left untouched.

The `.ns4p` programs and `.nsmp4` sample-library files are retained in the
imported bank but are not playable yet. The REA observations confirm the
container layout and manifest dependency links, but do not establish the
internal decoding or synthesis semantics of these binary formats. Enabling
them requires independent format decoding and then mapping program parameters
and dependencies into Classic Player's layer engine.
