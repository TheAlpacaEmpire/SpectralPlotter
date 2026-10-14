# Spectral Plotter

An audio-reactive topographic plotter rendered on a simulated phosphor CRT.

Spectral Plotter listens to whatever your PC is playing and turns the spectrum into a living
contour map — hills rise with the harmonics, a river cuts the valleys, percussion flashes
across the screen — drawn like an old war-room terminal: green phosphor, scanlines, bloom,
persistence and barrel distortion. The whole window is the screen; every readout, control
and label is part of the CRT image.

**[⬇ Download the latest release for Windows](https://github.com/TheAlpacaEmpire/SpectralPlotter/releases/latest)**

## Requirements

- Windows 10 or 11 (64-bit)
- A GPU with OpenGL 3.2 or newer

No installer and no runtime redistributables are needed: unzip and run `SpectralPlotter.exe`.

The app isn't code-signed, so Windows SmartScreen may say "Windows protected your PC" the first
time: click **More info → Run anyway**.

## Usage

By default the app captures the system's default playback device (WASAPI loopback), so just
play some music. Press **I** / **Shift+I** (or click the **SOURCE** arrows) to step through
other output and input devices — microphones and line-in work too.

| Key            | Action                                        |
| -------------- | --------------------------------------------- |
| `N`            | New terrain seed                              |
| `T` / `Shift+T` | Next / previous colour palette               |
| `L`            | Lock / unlock the hue                         |
| `I` / `Shift+I` | Next / previous audio source                 |
| `F` / `F11`    | Toggle fullscreen (`Esc` exits fullscreen)    |
| `[` / `]`      | Phosphor persistence down / up                |
| `-` / `=`      | Bloom down / up                               |
| `S`            | Save a screenshot of the scene                |
| `R` `R`        | Restore the factory look (press twice)        |

Every control is also clickable on the on-screen control panel: click a slider bar to set it,
or use its `< >` steppers. Drag the header bar to move the window; the header's `[+]` and
`[x]` toggle fullscreen and quit.

**Files**

- Settings (sliders, palette, audio source, window placement) are saved on exit to
  `%APPDATA%\SpectralPlotter\SpectralPlotter.settings`. **[ RESET ]** on the control panel (or
  `R` twice) restores the default sliders, palette and hue lock; deleting the file resets
  everything, including the audio source and window placement.
- Screenshots go to `Pictures\Spectral Plotter`.

## Building from source

Requires [JUCE](https://juce.com) 8 at `C:\JUCE` and Visual Studio 2026 (C++ desktop workload).

1. Open `CircularSpectrumAnalyzer/CircularSpectrumAnalyzer.jucer` in the Projucer (or run
   `Projucer.exe --resave` on it) to regenerate the Visual Studio project.
2. Build `CircularSpectrumAnalyzer/Builds/VisualStudio2026/SpectralPlotter.sln`, configuration
   **Release | x64**.

To produce the distributable zip, run:

```powershell
powershell -ExecutionPolicy Bypass -File package.ps1
```

This does a clean Release build and writes `dist/SpectralPlotter-<version>-win64.zip`.

## License

Spectral Plotter is free software, released under the GNU General Public License v3 — see
[LICENSE](LICENSE). It is built with the JUCE framework, used here under JUCE's GPLv3 terms.
