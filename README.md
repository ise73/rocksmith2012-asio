# Rocksmith (2012) ASIO — no Real Tone Cable needed

An ASIO patch for Ubisoft's original **Rocksmith** (2012, Steam version).
It lets the game use any ASIO audio interface for both guitar input and game audio output,
**with or without the Real Tone Cable**, and with much lower latency than the stock setup.

It is based on [rocksmith-on-linux](https://github.com/ferabreu/rocksmith-on-linux) by ferabreu,
which is itself a fork of [RS ASIO](https://github.com/mdias/rs_asio) by Micael Dias.
Configuration works the same way as in RS ASIO for *Rocksmith 2014*, so the RS ASIO
documentation and its per-interface examples apply here too — see
[Configuring your audio interface](#configuring-your-audio-interface) for the few differences.

Runs on **Windows** natively and on **Linux** through Proton/Wine (see [Linux](#linux-protonwine)).

## Contents

- [What it does](#what-it-does)
- [Installation (Windows)](#installation-windows)
- [Configuring your audio interface](#configuring-your-audio-interface)
- [Rocksmith 2012 specific settings](#rocksmith-2012-specific-settings)
- [Reducing latency](#reducing-latency)
- [Troubleshooting](#troubleshooting)
- [Linux (Proton/Wine)](#linux-protonwine)
- [How it works](#how-it-works)
- [Disclaimer](#disclaimer) · [Credits](#credits)

## What it does

- **Output:** game audio goes to your ASIO interface.
- **Input:** the guitar is read from an ASIO input channel.
- **No cable:** with `EmulateRealToneCable=1` the ASIO input is presented to the game as a
  Real Tone Cable. If a real cable is plugged in, emulation switches itself off and the
  cable is redirected to your ASIO input instead.
- **Low latency:** Rocksmith 2012 polls the audio device about every 10 ms instead of waiting
  for it. A ring buffer between the game and ASIO lets the driver run with small buffers
  (128–256 samples) without crackling.

Tested with: Steam Rocksmith (exe CRC32 `0xe0f686e0`), Windows 11 LTSC, Steinberg UR12,
with and without a Real Tone Cable plugged in. Other versions of the game are not supported
(the patch identifies the executable by its checksum).

## Installation (Windows)

1. Download the package: from the **Releases** page, or from the newest successful run on the
   [Actions page](../../actions/workflows/build.yml) (archive under **Artifacts**, GitHub login needed).
2. Copy `avrt.dll`, `RS_ASIO.dll` and `RS_ASIO.ini` into the game folder, next to `Rocksmith.exe`
   (Steam: *Library → Rocksmith → Manage → Browse local files*).
3. Edit `RS_ASIO.ini` for your interface — see the next section. At minimum set `Driver=` and `Channel=`.
4. In `Rocksmith.ini` (same folder) make sure the `[Audio]` section contains `ExclusiveMode=1`.
5. Start the game. If something doesn't work, look at `RS_ASIO.log` in the game folder.

To uninstall, delete `avrt.dll`, `RS_ASIO.dll`, `RS_ASIO.ini` and `RS_ASIO.log` from the game folder.

## Configuring your audio interface

RS ASIO's documentation covers the general setup in detail and is the place to look first:

- [How to use RS ASIO](https://github.com/mdias/rs_asio#how-to-use) and the
  [basic configuration guide](https://github.com/mdias/rs_asio#basic-configuration-guide)
  (finding your driver name, choosing buffer sizes, `LatencyBuffer`);
- [Audio interfaces reported to work well](https://github.com/mdias/rs_asio#audio-interfaces-reported-to-work-well),
  with ready-made `RS_ASIO.ini` examples for many interfaces in
  [RS ASIO's docs folder](https://github.com/mdias/rs_asio/tree/master/docs);
- [Known issues](https://github.com/mdias/rs_asio#known-issues) — driver-specific quirks
  (Focusrite buffer sizes, missing 32-bit drivers, ASIO4ALL, …) apply here as well.

Those examples are written for *Rocksmith 2014*. To use one with Rocksmith 2012, change it like this:

1. **`[Config]`**: set `EnableWasapiInputs=1` (Rocksmith 2012 needs it; 2014 examples use `0`).
2. **Guitar input goes in `[Asio.Input.0]`**: many 2014 examples leave `[Asio.Input.0]` empty
   (`;Driver=`) and put the guitar in `[Asio.Input.1]`. Move the `Driver=` and `Channel=` of the
   guitar input to `[Asio.Input.0]`.
3. **Add to `[Asio.Input.0]`**:
   ```ini
   EmulateRealToneCable=1
   WasapiDevice=Rocksmith
   ```
4. **Add to `[Asio]`** (optional, these are the defaults):
   ```ini
   EnablePollingRingBuffer=1
   PollingBufferSize=
   ```
5. `GameSampleRate44100` and other options tied to specific *Rocksmith 2014* builds are not
   available; your interface must run at 48 kHz.

Example for a Steinberg UR12 (guitar in the Hi-Z input, which is ASIO input 2 → `Channel=1`):

```ini
[Config]
EnableWasapiOutputs=0
EnableWasapiInputs=1
EnableAsio=1

[Asio]
BufferSizeMode=driver
CustomBufferSize=
EnablePollingRingBuffer=1
PollingBufferSize=512

[Asio.Output]
Driver=Yamaha Steinberg USB ASIO
BaseChannel=0
EnableSoftwareEndpointVolumeControl=1
EnableSoftwareMasterVolumeControl=1
SoftwareMasterVolumePercent=100

[Asio.Input.0]
Driver=Yamaha Steinberg USB ASIO
Channel=1
EnableSoftwareEndpointVolumeControl=1
EnableSoftwareMasterVolumeControl=1
SoftwareMasterVolumePercent=100
WasapiDevice=Rocksmith
EmulateRealToneCable=1
```

Driver buffer size (Yamaha Steinberg USB Driver control panel): 128.

If you get your interface working, please open an issue with your `RS_ASIO.ini` so it can be listed here.

## Rocksmith 2012 specific settings

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[Asio.Input.0]` | `EmulateRealToneCable` | `0` | `1` = show this ASIO input to the game as a Real Tone Cable. Ignored while a real cable is connected. Only one emulated instrument has been tested. |
| `[Asio.Input.0]` | `WasapiDevice` | — | With a real cable plugged in: the WASAPI device to redirect to this ASIO input. `Rocksmith` matches the cable. Matched case-insensitively against the device name and ID. |
| `[Asio]` | `EnablePollingRingBuffer` | `1` | Ring buffer between the game and ASIO. `0` = old behaviour, which needs a driver buffer of about 512 samples. |
| `[Asio]` | `PollingBufferSize` | empty = auto | Output ring buffer size in samples. Smaller = less latency; too small = crackling. |
| `[Config]` | `DiagnosticLog` | `0` | `1` = log the game's low-level device calls. Only for troubleshooting; makes the log large. |

All other keys work as described in [RS ASIO](https://github.com/mdias/rs_asio).

## Reducing latency

Total latency is roughly the driver buffer (input and output) plus the output ring buffer plus
the game's own `LatencyBuffer`. Tune in this order, playing for a couple of minutes after each change:

1. **Driver buffer** in your interface's control panel: try 128, then 64 if your interface allows it.
   Go back up (256) if you hear crackling.
2. **`PollingBufferSize`** in `RS_ASIO.ini`: with a 128-sample driver buffer, `512` is a good value
   (the automatic value is 768). Values below about 480 are unlikely to work, because the game writes about 10 ms (480 samples) at a time.
3. **`LatencyBuffer`** in `Rocksmith.ini` (`[Audio]` section): try 4, 3, 2.

Crackling shows up in `RS_ASIO.log` as `polling FIFO underrun` lines.

## Troubleshooting

| Problem | What to check |
|---|---|
| Game asks you to connect the cable | `EmulateRealToneCable=1` is in `[Asio.Input.0]`, `EnableWasapiInputs=1` in `[Config]`, and `RS_ASIO.log` contains `will be presented to the game as a Real Tone Cable`. |
| "Too many instruments connected" | Update to the latest build (older builds kept emulating while a real cable was plugged in). |
| No guitar sound | `Channel=` counts from 0; check the input gain on your interface. `RS_ASIO.log` lists the ASIO input channel names. |
| No game sound / game won't start | The `Driver=` name must match a line under `AsioHelpers::FindDrivers` in `RS_ASIO.log` exactly. The driver must be 32-bit. |
| Crackling | Raise the driver buffer or `PollingBufferSize` (see [Reducing latency](#reducing-latency)). |
| "Did you set ExclusiveMode=1" message | Add `ExclusiveMode=1` to the `[Audio]` section of `Rocksmith.ini`. |

Driver-specific problems are usually the same as in Rocksmith 2014 — see RS ASIO's
[known issues](https://github.com/mdias/rs_asio#known-issues) and
[issue tracker](https://github.com/mdias/rs_asio/issues).

## Linux (Proton/Wine)

The Linux setup from rocksmith-on-linux is kept as is. The package's `linux/` folder contains
its `RS_ASIO.ini` (preconfigured for PipeASIO), `rocksmith-on-linux.sh`, `pipeasio-register` and the
[setup guide](docs/setup-guide.md).

Requirements:

- PipeWire and [PipeASIO](https://github.com/M0n7y5/pipeasio) (WineASIO with JACK also works, see the setup guide);
- Steam with Proton, and the Steam version of Rocksmith;
- the Real Tone Cable: on Linux, `EmulateRealToneCable` has not been tested yet.

Installation: copy `avrt.dll` and `RS_ASIO.dll` from the package root, and `RS_ASIO.ini` and
`rocksmith-on-linux.sh` from `linux/`, into the game folder, then follow the
[setup guide](docs/setup-guide.md). Known Linux issues and the Wine/Proton specifics are described in
the setup guide and in [tech-details.md](docs/tech-details.md).

## How it works

The DLL is loaded through a proxy `avrt.dll` and patches the game's import address table.
WASAPI device enumeration is replaced so the game sees ASIO-backed devices.

To recognise the cable, the game (1) inspects capture endpoints whose form factor is
*Microphone*, looking for a property that contains `VID_12BA&PID_00FF`, and (2) scans
`KSCATEGORY_AUDIO` devices with SetupAPI. Emulation answers both: the ASIO input reports the
cable's form factor, name and USB ID, and a synthetic cable interface is appended to the
SetupAPI scan. Its KS handle is the NUL device; the game tolerates the failed KS queries.

More details, including the Wine-specific parts: [docs/tech-details.md](docs/tech-details.md).

## Disclaimer

- Made for personal use and tested on a small number of setups; it may not work with yours.
- Provided as-is, without warranty of any kind. Use at your own risk. See [LICENSE](LICENSE).
- *Rocksmith* is a trademark of Ubisoft. This project is independent and not affiliated with,
  endorsed by, or approved by Ubisoft.

## Credits

- [RS ASIO](https://github.com/mdias/rs_asio) — Micael Dias
- [rocksmith-on-linux](https://github.com/ferabreu/rocksmith-on-linux) — ferabreu (Rocksmith 2012 support, Linux)
- Windows port, ring buffer and Real Tone Cable emulation — ise73, with Claude (Anthropic)

MIT license, see [LICENSE](LICENSE).
