# Rocksmith (2011) ASIO — no Real Tone Cable needed

An ASIO patch for Ubisoft's original **Rocksmith** (2011/2012, Steam version) on **Windows**.
It lets the game use any ASIO audio interface for both guitar input and game audio output,
**with or without the Real Tone Cable**, and with much lower latency than the stock WASAPI setup.

It is based on [rocksmith-on-linux](https://github.com/ferabreu/rocksmith-on-linux) by ferabreu,
which is itself a fork of [RS ASIO](https://github.com/mdias/rs_asio) by Micael Dias.
Linux/Proton users: the Linux setup is still supported, see [docs/README.md](docs/README.md).

## What it does

- **Output:** game audio goes to your ASIO interface.
- **Input:** guitar is read from an ASIO input channel.
- **No cable:** with `EmulateRealToneCable=1` the ASIO input is presented to the game as a
  Real Tone Cable. If a real cable is plugged in, emulation switches itself off and the
  cable's input is redirected to ASIO instead (as in rocksmith-on-linux).
- **Low latency:** Rocksmith 2011 polls the audio device about every 10 ms. A ring buffer
  between the game and ASIO lets the driver run with small buffers (128–256 samples)
  without crackling.

Tested with: Steam Rocksmith (exe CRC32 `0xe0f686e0`), Windows 11 LTSC, Steinberg UR12.

## Installation

1. Download the latest build: open the
   [Actions page](../../actions/workflows/build.yml), pick the newest successful run of the
   `windows-port`/`main` branch and download the archive under **Artifacts**
   (or use a release, once there is one).
2. Copy `avrt.dll` and `RS_ASIO.dll` into the game folder (next to `Rocksmith.exe`).
3. Copy `RS_ASIO-windows.ini` there too and **rename it to `RS_ASIO.ini`**.
4. Edit `RS_ASIO.ini`:
   - `Driver=` in `[Asio.Output]` and `[Asio.Input.0]`: the exact name of your ASIO driver.
     If unsure, start the game once and look for `AsioHelpers::FindDrivers` in `RS_ASIO.log`.
   - `Channel=` in `[Asio.Input.0]`: the input your guitar is plugged into, counting from 0
     (e.g. on a Steinberg UR12 the Hi-Z guitar input is `1`).
5. In `Rocksmith.ini` make sure the `[Audio]` section has `ExclusiveMode=1`.
6. In your ASIO driver's control panel set the buffer size to **128** or **256** samples.

To uninstall, delete `avrt.dll`, `RS_ASIO.dll`, `RS_ASIO.ini` and `RS_ASIO.log` from the game folder.

## Configuration reference (Rocksmith 2011 specific)

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[Asio.Input.N]` | `EmulateRealToneCable` | `0` | `1` = show this ASIO input to the game as a Real Tone Cable. Ignored while a real cable is connected. |
| `[Asio.Input.N]` | `WasapiDevice` | — | With a real cable: name/ID of the WASAPI device to redirect to this ASIO input. `Rocksmith` matches the cable. |
| `[Asio]` | `EnablePollingRingBuffer` | `1` | Ring buffer between game and ASIO. `0` = old behaviour (needs a 512-sample driver buffer). |
| `[Asio]` | `PollingBufferSize` | auto | Ring buffer size in samples. Smaller = less latency; too small = crackling. `512` works well at a 128-sample driver buffer. |
| `[Config]` | `DiagnosticLog` | `0` | `1` = log the game's low-level device calls. Only for troubleshooting. |

All other keys work as in [RS ASIO](https://github.com/mdias/rs_asio).

## Troubleshooting

- **Crackling:** increase the driver buffer (256), or set `PollingBufferSize=768`/`1024`.
  `RS_ASIO.log` shows `polling FIFO underrun` lines when this happens.
- **"Too many instruments connected":** you are on an old build; update. Current builds
  disable cable emulation while a real cable is plugged in.
- **Game asks for the cable:** check that `EmulateRealToneCable=1` is in the input section
  and that `RS_ASIO.log` contains `will be presented to the game as a Real Tone Cable`.
- **No guitar sound:** check `Channel=` and the input gain on your interface.

## How it works (short)

The DLL is loaded through a proxy `avrt.dll` and patches the game's import address table.
WASAPI device enumeration is replaced so the game sees ASIO-backed devices.
To recognise the cable the game (1) inspects capture endpoints with form factor *Microphone*
for a property containing `VID_12BA&PID_00FF`, and (2) scans `KSCATEGORY_AUDIO` with SetupAPI.
Emulation answers both: the ASIO input reports the cable's form factor, name and USB ID, and
a synthetic cable interface is appended to the SetupAPI scan (its KS handle is a NUL device;
the game tolerates failed KS queries). Details: [docs/tech-details.md](docs/tech-details.md).

## Credits

- [RS ASIO](https://github.com/mdias/rs_asio) — Micael Dias
- [rocksmith-on-linux](https://github.com/ferabreu/rocksmith-on-linux) — ferabreu
- Windows port, ring buffer and cable emulation: ise73, with Claude (Anthropic)

*Rocksmith* is a trademark of Ubisoft. This project is not affiliated with or endorsed by Ubisoft.
MIT license, see [LICENSE](LICENSE).
