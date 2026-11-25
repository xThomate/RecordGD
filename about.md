# RecordGD

Automatically saves a video of your run when you beat a level.

Every attempt is recorded in the background, and attempts you don't finish are never saved. When you complete the level, a **Save Run** button appears on the end screen: press it to keep the video (with game audio) in your recordings folder, together with a small `.json` file describing the run. You can also have every completion saved automatically.

> **AI-assisted project:** RecordGD was developed with the help of an AI assistant (Claude).

## Features
- Records automatically: just play, and your winning run is waiting for you on the end screen
- Optionally starts the video a few seconds before the winning attempt, so it opens on your last death
- Records music and sound effects, with optional microphone recording mixed in
- Keeps recording a few seconds after you finish to include the end animation
- Hardware encoding (NVIDIA / AMD / Intel) with automatic fallback to software
- No extra downloads: everything it needs is built into Windows
- Optional: save practice-mode runs, start-position runs, or only first completions

## Notes
- Windows only.
- Videos are saved to `recordings` in the mod's save folder unless you choose another folder in the settings.
- Audio is recorded at your in-game volume. If music is muted in-game, it will be silent in the video.
- Pause menus and level transitions are cut out of the video.
- Microphone recording is off by default. When it's on, a notification shows which device is being recorded.
