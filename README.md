# MemGlow

English | [简体中文](README.zh-CN.md)

Visualize a program's memory in real time.

Pretty useless, but it looks cool and lets you live out your hacker fantasies.

![MemGlow visualizing Space Cadet Pinball's memory](docs/images/demo.gif)

![MemGlow visualizing Wolfenstein 3D's memory](docs/images/wolf3d-demo.gif)

## Usage

1. Download the Windows x64 ZIP from GitHub Releases and extract it. Keep `MemGlow.exe` and `SDL2.dll` in the same folder.
2. Start the program you want to view. Find its PID in Task Manager → **Details**.
3. Open PowerShell in the extracted folder and run:

   ```powershell
   .\MemGlow.exe -p 1234 --all
   ```

Replace `1234` with the target program's PID. If access is denied, try running PowerShell as administrator.

## Controls

| Control | What it does |
|---|---|
| Mouse wheel | Zoom in or out in any of the three views. Hold Shift while scrolling for smaller steps. |
| Hold the left mouse button and drag vertically | Pan the main view or move a selection box in the left-hand strips. |
| Drag a selection's top/bottom edge | Resize it. Drag outside the box to select a new range. |
| Click `A ALL` / press `A` | Show all memory. |
| Click `T COLOR` / press `T` | Switch between green and category colors. |
| Click `F GLOW` / press `F` | Toggle the glow on changing data. |
| `F11` | Toggle fullscreen. |
| Escape | Leave fullscreen; close the program when windowed. |

Use `--help` to see the other command-line options.

## Credits

Inspired by Christopher Domas's talk [The Future of RE: Dynamic Binary Visualization](https://recon.cx/2013/schedule/events/20.html).

The demos use [Space Cadet Pinball](https://github.com/k4zmu2a/SpaceCadetPinball) and Wolfenstein 3D ([DDWolf](https://github.com/brosseaualex/DDWolf)). The games are not included in the release package.
