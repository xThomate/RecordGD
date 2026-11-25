<p align="center">
  <img src="logo.png" width="128" alt="RecordGD logo">
</p>

<h1 align="center">RecordGD</h1>

<p align="center">
  A <a href="https://geode-sdk.org">Geode</a> mod for Geometry Dash that records every attempt in the background<br>
  and saves a video when you beat the level.
</p>

---

## Features

- **Records automatically:** every attempt is recorded in the background, and only completed runs are kept.
- **Save Run button:** a button on the end screen lets you decide whether to keep the video, or you can save every completion automatically.
- **Your last death, too:** the video can optionally start a few seconds before the winning attempt.
- **Game audio and microphone:** music and sound effects are recorded as you hear them, with optional microphone recording mixed in.
- **Light on performance:** hardware H.264 encoding (NVIDIA / AMD / Intel) through Windows Media Foundation, with automatic fallback to software. No extra downloads.
- **Clean videos:** pause menus and level transitions are cut out, and recording continues for a few seconds after you finish to include the end animation.

## Installation

1. Install [Geode](https://geode-sdk.org) (v6, Geometry Dash 2.2091).
2. Download `thomasch.recordgd.geode` from the [Releases](../../releases) page, or from the latest successful run on the [Actions](../../actions) tab.
3. Put it in Geometry Dash's `geode/mods` folder and restart the game.

## Where videos are saved

By default:

```
%LOCALAPPDATA%\GeometryDash\geode\mods\thomasch.recordgd\recordings\
```

You can change this with the **Output Folder** setting. Each video is named like `Stereo Madness (1) 2026-11-25 19-30-12 57att.mp4` and comes with a `.json` file describing the run (attempts, run time, resolution, audio).

## Settings

| Setting | Default | |
|---|---|---|
| Enabled | on | |
| Output Folder | *(empty)* | Empty = `recordings` in the mod's save folder |
| Show Notifications | on | |
| Ask Before Saving | on | Save Run button on the end screen instead of auto-saving |
| Resolution | 1080p | native / 1440p / 1080p / 720p / 480p; never upscales |
| FPS | 60 | |
| Bitrate (Mbps) | 20 | |
| Hardware Encoding | on | Falls back to software automatically |
| Record Audio | on | Recorded at your in-game volume |
| Seconds Before Attempt | 0 | e.g. 3 = the video opens on your last death |
| Seconds After Completion | 3 | Includes the end animation |
| Record Microphone | off | |
| Microphone Volume | 1.0 | 0–4 |
| Microphone Device | 0 | 0 = system default; the numbered device list is written to the Geode log |
| Save Practice Runs | off | |
| Save Start Position Runs | off | |
| First Completion Only | off | |

## Building

Requirements: Windows, Visual Studio 2022, the [Geode SDK](https://docs.geode-sdk.org/getting-started/) **6.0.0** and the Geode CLI.

```sh
geode build
```

The mod has no dependencies on other mods: video and audio are encoded with Windows Media Foundation (`mfplat`, `mfreadwrite`, `mfuuid`), which ships with Windows.

Every push is also built by GitHub Actions (`.github/workflows/build.yml`), and the resulting `.geode` file is attached to the workflow run.

## How it works

```
startGame ──► RunRecorder.start()  ──► new session (one encoder + one temp .mp4 for the whole level visit)
resetLevel ─► RunRecorder.start()  ──► same session; marks "attempt starts at frame F" + forces a keyframe
        │
        ├─ every frame: CCEGLView::swapBuffers ──► FrameCapture (GPU blit + async PBO readback, BGRA)
        │                                         └► Encoder thread ──► Media Foundation sink writer (H.264)
        │               FMOD master DSP tap ──────────► Encoder thread ──► same .mp4 (AAC)
        │               FMOD record driver (mic) ──────► resample ──► mixed into the game audio
        │
        ├─ deaths keep recording (the death can become pre-roll); quit / practice ──► discard() ──► temp file deleted
        │
        └─ levelComplete ──► Tail (N more seconds) ──► finalize()
                                ├─ cut [F − pre-roll, end] out of the session (passthrough remux, no re-encode)
                                ├─ auto mode: Finalize() + move ──► <output folder>/<Level> (<ID>) <date> <N>att.mp4 + .json
                                └─ ask mode:  Finalize(), file held in temp ──► end-screen "Save Run" ──► Save: move / Discard: delete
                                              (leaving the end screen without answering = discard)
```

| File | Role |
|---|---|
| `src/main.cpp` | Geode hooks on `PlayLayer`, `EndLevelLayer` (Save Run button + popup) and `CCEGLView` |
| `src/RunRecorder.*` | Main-thread state machine (Idle → Recording → Tail), sessions and attempt markers, constant-FPS pacing, back-pressure |
| `src/FrameCapture.*` | OpenGL back-buffer capture: FBO blit (downscale) + 3 pixel-buffer objects for async readback |
| `src/AudioCapture.*` | Pass-through FMOD DSP on the master channel group (music + SFX as heard); also the audio clock |
| `src/MicCapture.*` | Microphone via FMOD record drivers: ring buffer polled each frame, mono downmix, linear resample to the game rate |
| `src/Encoder.*` | Worker thread: Media Foundation sink writer (H.264 + AAC in one .mp4), hardware → software fallback, keyframe control (`ICodecAPI`), passthrough remux to cut the run out of the session, saving |
| `src/Output.*` | File names, sidecar JSON, temp cleanup, notifications, `asp::fs::Path` conversion |
| `src/Settings.*` | Snapshot of the mod settings |
| `logo.png`, `resources/*.png` | Mod icon and end-screen button icons, rendered from the sources in `art/` |

### Details

- **Video clock:** advances with real time, but only while the level is visible and unpaused, so pause menus and scene transitions are cut out. Each captured image is written as many times as needed to hold a constant FPS. Audio is gated by the same condition, so the two stay in sync.
- **Sessions:** one encoder runs for the whole visit to a level. A fresh attempt only marks a frame number and asks the encoder for a keyframe, so restarting costs nothing and the end of the previous attempt stays available.
- **Pre-roll (Seconds Before Attempt):** the cut lands on the nearest keyframe at or before the requested point (keyframes are forced at each attempt start and placed every second otherwise), so the video can start up to about 1 s earlier than set. It never reaches back past the start of the session.
- **Session length cap:** once a session holds 5 minutes of footage, the next attempt starts a new session to keep the temp file around 750 MB (at 20 Mbps). That attempt has no pre-roll. Changing video/audio settings or the window size between attempts also starts a new session.
- **Checkpoints:** respawns at a practice or platformer checkpoint aren't new attempts; the run continues.
- **Encoder fallback:** tries hardware H.264 + AAC, then software, then each without audio. If hardware fails once, the rest of the game session uses software.
- **Back-pressure:** if the encoder can't keep up, frames are skipped (the previous image is held longer) instead of slowing the game. The count is written to the `.json` as `droppedFrames`.
- **Save Run button:** added to the end screen's row of big buttons (found at runtime, since the end screen has no node IDs in Geode 6) and sized to match them. If the row can't be found, it goes in the top-right corner. Pressing it stops recording immediately, so the popup isn't in the video. Escape closes the popup without deciding; leaving without answering discards the run.
- **Microphone:** mixed into the game audio with its own volume. The mic and the game run on different clocks, so the mic queue is kept at most 300 ms deep. With "Record Audio" off but the mic on, only the mic is heard.
- **Audio fallback:** if audio fails (unsupported sample rate, encoder error), the video is still saved without it. AAC only supports 44.1 kHz and 48 kHz.

## Known limitations

- Windows only (capture uses OpenGL through GLEW, encoding uses Media Foundation).
- Overlays drawn by other mods before `swapBuffers` (some mod menus) can appear in the video, depending on hook order.
- Closing the game while a video is being saved loses that video. Leftover temp files are cleaned up on the next launch.

## License

[MIT](LICENSE) © thomasch
