# SkyRoof → N3FJP AC Log Plugin

A logger plugin for [SkyRoof](https://ve3nea.github.io/SkyRoof/) by VE3NEA that sends
your satellite QSOs directly to **N3FJP's Amateur Contact Log (AC Log)**, with callbook
lookup, so you never have to retype a contact.

Written by Keith, KV5J.

## What it does

- Sends each QSO logged in SkyRoof straight to AC Log
- Includes the satellite details AC Log and LoTW need: satellite name, propagation mode SAT, frequencies, mode, and grid
- Looks up the station in the callbook so name, QTH, and other details fill in automatically
- Works with AC Log's normal LoTW upload, so your satellite contacts confirm like any other QSO

## Requirements

- Windows
- SkyRoof version 1.55 or later
- N3FJP Amateur Contact Log version 7.0.12.1 or later
- No changes are needed to AC Log except those listed below

## Installation

1. Download the latest zip from the
   [Releases page](https://github.com/KV5J/SkyRoof-ACLog-Plugin/releases/latest).
2. **Unblock the zip before extracting it.** Right-click the zip file, choose
   **Properties**, check **Unblock** at the bottom, and click **OK**.
   (Windows blocks downloaded DLLs, and SkyRoof may silently ignore a blocked plugin.)
3. Close SkyRoof.
4. Copy `LoggerInterface.dll` and `LoggerInterface.ini` into the SkyRoof main program folder.
   They replace the existing files, so keep a backup of the originals.
5. Start SkyRoof.

## Setting up AC Log

1. In AC Log, open **Settings → Application Program Interface (API)**.
2. Check **TCP API Enabled**. Leave the port at **1100** unless you've changed it.

## Setting up SkyRoof

No setup changes are needed.

## Settings (LoggerInterface.ini)

The defaults work for most people. Edit the file with Notepad if you need to change anything.

| Setting | Default | What it does |
|---|---|---|
| Host | 127.0.0.1 | Address of the PC running AC Log (leave as-is if it's the same PC) |
| Port | 1100 | AC Log's TCP API port |
| LookupWaitMs | 2000 | How long (in milliseconds) to wait for AC Log's callbook lookup before saving the QSO |
| NewFileEvery | Year | How often the plugin starts a new file |
| Debug | 0 | Set to 1 to turn on debug logging for troubleshooting |

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
- **Callbook info is missing.** Check that callbook lookup is set up and working in AC Log
  itself. On a slow connection, try raising `LookupWaitMs` to 3000 or more.

## Reporting problems

Please open an [Issue](https://github.com/KV5J/SkyRoof-ACLog-Plugin/issues) and include
your SkyRoof and AC Log version numbers and a description of what happened.

## Thanks

To Alex, VE3NEA, for SkyRoof and its plugin interface, and to Scott, N3FJP, for AC Log
and its API.

## License

MIT. See the [LICENSE](LICENSE) file. Free to use, modify, and share.

73 de KV5J
