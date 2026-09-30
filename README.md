# SkyRoof → N3FJP AC Log Plugin

A logger plugin for [SkyRoof](https://ve3nea.github.io/SkyRoof/) by VE3NEA that sends
your satellite QSOs directly to **N3FJP's Amateur Contact Log (AC Log)**, with callbook
lookup, so you never have to retype a contact.

Written by Keith, KV5J.

## What it does

- Sends each QSO logged in SkyRoof straight to AC Log
- Includes the satellite details AC Log and LoTW need: [satellite name, propagation mode SAT, frequencies, mode, grid]
- Looks up the station in the callbook so name, QTH, and other details fill in automatically
- Works with AC Log's normal LoTW upload, so your satellite contacts confirm like any other QSO

## Requirements

- Windows
- SkyRoof version [1.52] or later
- N3FJP Amateur Contact Log version [x.x] or later
- [Callbook subscription, e.g. QRZ XML, if required for lookups]

## Installation

1. Download the latest zip from the
   [Releases page](https://github.com/KV5J/SkyRoof-ACLog-Plugin/releases/latest).
2. **Unblock the zip before extracting it.** Right-click the zip file, choose
   **Properties**, check **Unblock** at the bottom, and click **OK**.
   (Windows blocks downloaded DLLs, and SkyRoof may silently ignore a blocked plugin.)
3. Close SkyRoof.
4. Copy `LoggerInterface.dll` into [the SkyRoof plugin folder, e.g. `C:\...\SkyRoof\Plugins`].
   [If it replaces an existing file, say so and suggest keeping a backup.]
5. Start SkyRoof.

## Setting up AC Log

1. In AC Log, open **Settings → Application Program Interface (API)**.
2. Check **TCP API Enabled**. Leave the port at **[1100]** unless you've changed it.
3. [Any other AC Log settings needed.]

## Setting up SkyRoof

1. [Steps to select or enable the plugin in SkyRoof.]
2. [Where to enter the AC Log address/port, if needed. Default: `127.0.0.1`, port `[1100]`.]

## Using it

Log your QSO in SkyRoof as usual. It should appear in AC Log within a second or two,
with callbook details filled in. Upload to LoTW from AC Log as you normally do.

## Troubleshooting

- **Nothing appears in AC Log.** Make sure AC Log is running *before* you log the QSO,
  and that the TCP API is enabled on the correct port.
- **SkyRoof doesn't seem to load the plugin.** Check that you unblocked the zip (step 2
  of Installation), then re-copy the DLL.
- **Antivirus warning.** Some antivirus programs flag unsigned DLLs. The full source code
  is in this repository if you'd like to review it or build it yourself.
- **Callbook info is missing.** [Check callbook login/subscription settings.]

## Reporting problems

Please open an [Issue](https://github.com/KV5J/SkyRoof-ACLog-Plugin/issues) and include
your SkyRoof and AC Log version numbers and a description of what happened.

## Building from source

[Visual Studio version / .NET version needed, and which project file to open.]

## Thanks

To Alex, VE3NEA, for SkyRoof and its plugin interface, and to Scott, N3FJP, for AC Log
and its API.

## License

MIT. See the [LICENSE](LICENSE) file. Free to use, modify, and share.

73 de KV5J
