# Linc Robot V1 board, radio player, web control page

Gap analysis against the feature list of a Vietnamese robot-kit vendor (its "V1" kit). Adapted to the owner's real hardware: ESP32-S3 N16R8, DRV8833 motor driver, two LEDs, a 0.96" song OLED and a 1.3" emotion OLED, and what was added. **Nothing here has been compiled or run on hardware.** No ESP-IDF toolchain was available.

## Gap analysis

| Site feature | State in this repo before | What was done |
|---|---|---|
| Vietnamese voice chat | Already supported: `vi-VN` locale, server-side ASR/LLM | Nothing needed |
| Wake word "Hi LiLy" | Custom wake word is supported (`CONFIG_USE_CUSTOM_WAKE_WORD`, `CONFIG_CUSTOM_WAKE_WORD`) | **Not done.** A trained wake word model for "Hi LiLy" is needed; it cannot be generated here |
| Music by song name | Not present | **Added.** `self.music.play` searches **Jamendo** (free, Creative Commons, free API key) and/or a music server you configure (the fork's protocol, ported), and tries the next result when one cannot be played. the same tool plays a direct URL when `url` is given |
| Radio VOV1 / VOV2 / VOV Giao thong | Not present | **Added.** `RadioPlayer` plays HLS and plain streams. 14 channels: vov1, vov2, vov3, vovgt_hn, vovgt_hcm (URLs checked) plus 9 more from the Vietnam fork (URLs not re-checked). Spoken names like "VOV giao thong Ha Noi" or "dai Tay Nguyen" are matched too |
| AI tools (weather, gold, news, translation) | Server-side MCP tools | Nothing to add on the device. The new tools below can be called by the same server |
| Wheel control (D-pad, dance) | Only servo robots (Otto, Electron Bot). No DC motor driver | **Added.** `Drv8833Motor`, `RobotController`, MCP tools, web D-pad |
| OLED faces from the web | Faces exist; no way to set them remotely | **Added.** Web buttons plus `self.robot.set_emotion` |
| OLED audio spectrum | Not present | **Added** on the 0.96" song OLED: 16-band FFT of the PCM being played, plus the song title (see below) |
| Web config (Wi-Fi, status, OTA, system info) | Wi-Fi setup page only (esp-wifi-connect) | **Added.** Status page with version, Wi-Fi, IP, state, heap, uptime and an "update firmware" button |
| OTA over Wi-Fi | Already supported (`ota.cc`, `Application::UpgradeFirmware`) | Reused by the web button |
| Flash from Chrome/Edge | Not present | **Added.** `docs/web-flasher/` (ESP Web Tools page + manifest) |
| Board for this robot | Not present | **Added.** `main/boards/linc-robot-v1/` (Kconfig `BOARD_TYPE_LINC_ROBOT_V1`) |

## Changes

New:
- `main/radio/radio_player.{h,cc}`: internet radio / URL player (fetch task + decode task, PSRAM stream buffer, `esp_audio_simple_dec` for AAC / MP3 / MPEG-TS, resample, mono downmix).
- `main/boards/linc-robot-v1/`: `config.h`, `config.json`, `linc_robot_board.cc`, `drv8833_motor.*`, `song_display.*`, `robot_controller.*`, `robot_web_server.*`.
- `docs/web-flasher/`.

Edited:
- `main/audio/audio_service.{h,cc}`: `PushPcmToPlaybackQueue()`, the only way decoded PCM enters the existing playback queue.
- `main/Kconfig.projbuild`, `main/CMakeLists.txt`: board entry, `radio/radio_player.cc`, `radio` include dir.

## MCP tools (voice)

| Tool | Purpose |
|---|---|
| `self.robot.move` | `direction` forward/backward/left/right, `speed` 0-100, then ONE of: `duration_ms` 100-5000 (default), `distance_cm` 1-300 (forward/backward: "go forward 5 cm"), `degrees` 1-360 (left/right: "turn right 90 degrees"). Slows down and stops by itself |
| `self.robot.stop` | Stops the wheels at once and cancels a dance |
| `self.robot.lights` | `mode` left / right / both / alternate / fast / slow / off, `duration_ms` 500-30000: blink the two signal lights |
| `self.robot.dance` | `speed` 0-100 |
| `self.robot.set_emotion` | One of the 27 faces |
| `self.robot.get_status` | IP, Wi-Fi, wheels, radio |
| `self.radio.play` | `name`: built-in channel id, a spoken channel name, any other station name or kind of radio (free directory search, with fallback to the next station), or a stream URL; `country` (default VN), `tag` |
| `self.music.play` | `song` + optional `artist`: search Jamendo and/or the configured music server, first result that works; or `url`: a direct MP3 / AAC / HLS link |
| `self.media.control` | `action` `next` (next station or track) or `stop` |

These are 9 more tools (they were 13 before being merged). The cloud side accepts a bounded number of tools and drops the rest quietly, so check `LogToolInventory()` in the boot log.

## Exact distance and angle (open loop)

There are no wheel encoders, so "5 cm" is a **time**: `cm / (cm_per_s_full * speed / 100)`, and an angle is `degrees / (deg_per_s_full * speed / 100)`. The speed ramp is symmetric (what is lost while accelerating is regained while slowing down), so no correction is needed. Defaults (35 cm/s and 300 deg/s at 100 %) are guesses: **calibrate on your floor**.

Calibration (web page, "Đi theo quãng đường / góc quay" > "Hiệu chỉnh"): press "Chạy thử 2 giây", measure how far the robot went, enter it, Save. Same for the spin test (1 s at 70 %). The two numbers are stored in NVS (`robot` / `cal_cm_x10`, `cal_deg`) and survive reboots. Expect an error of a few centimetres / degrees: it changes with battery charge, floor and load. Re-calibrate after changing the battery or wheels. A limit of 12 s per command applies (about 250 cm at full speed). Real accuracy needs wheel encoders (not fitted).

## Own hotspot (phone control without the router)

While the robot is connected to the router it also broadcasts **`Linc-Robot-XXXX`** (WPA2, XXXX = end of its MAC address, default password **`12345678`**). A phone that joins it opens **http://192.168.4.1:8080/** (the address moves to `192.168.77.1` when the router itself uses 192.168.4.x). The phone gets no Internet through the robot; the robot's own voice chat, radio and music keep using the router. The SSID and password are also shown on the robot's screen when the hotspot starts, and in the log.

Change the password, or turn the hotspot off, on the web page ("Wi-Fi của robot", needs the admin key) or in NVS (`robot` / `ap_pass` 8-63 characters, `ap_on`). **Change the default password** if others can reach the robot. Up to 3 phones. The hotspot goes away while the Wi-Fi setup portal is open and comes back when the robot reconnects. The channel is the router's channel (one radio).

## Waking by voice

Choose a wake word when building: in the Builder "Wake word" = `(auto)` (picks `hiesp` = "Hi, ESP" on this board) or `hiesp` / `nihaoxiaozhi`; with the command line, `--wake-word hiesp`. `disabled` means only a button can wake the robot, and this robot has none wired. "Hi LiLy" needs a trained model and is not available. There is no echo cancellation: loud radio or music from the speaker can trigger the wake word or hide your voice, so lower the volume or say the wake word close to the microphone.

## Web page

`http://<robot-ip>:8080/` (the IP is in the boot log and in `self.robot.get_status`).

- D-pad: hold to move, release to stop. Moves carry a 400 ms duration and are refreshed every 250 ms, so a lost link stops the robot.
- State-changing calls are POST with an `X-Robot` header. Driving, radio and faces use an optional token (NVS `robot` / `web_token`); the page asks for it on first use.
- **Admin key** for the sensitive actions: choosing, adding or deleting a server (agent), the music source (Jamendo id, music server) and firmware check / upload. It is 8 hex characters, created at the first boot (NVS `robot` / `ota_token`) and shown **only on the robot**: the song OLED while idle, a notification on the face screen when the web server starts, and the log. Five wrong keys block admin calls for a minute. Without it, someone on the LAN could point the robot at their own server and have it install their firmware.
- The server stops while the Wi-Fi setup hotspot is up (that network is open).
- LAN only. There is no TLS. Do not expose port 8080 to the internet.

## Hardware map

| Part | Pins | Source |
|---|---|---|
| DRV8833 channel A (left wheel) | IN1 = GPIO12, IN2 = GPIO13 | owner |
| DRV8833 channel B (right wheel) | IN1 = GPIO14, IN2 = GPIO21 | owner |
| Turn signal, LEFT | GPIO46 (blinks while the robot turns left; also both lights when it drives straight) | owner pins, side assumed |
| Turn signal, RIGHT | GPIO48 (blinks while the robot turns right) | owner pins, side assumed |
| Song OLED 0.96" SSD1306 | I2C: SDA = GPIO41, SCL = GPIO42, address 0x3C | pins owner, address assumed |
| Emotion TFT 1.3" IPS ST7789 240x240 (7-pin SPI module) | SCL = GPIO9, SDA (MOSI) = GPIO10, RES = GPIO18, DC = GPIO8, BLK (backlight) = GPIO17; no CS pin (SPI mode 3) | owner |
| Emotion OLED 1.3" (alternative, `EMOTION_DISPLAY_TFT` = 0) | same I2C bus as the song OLED, address 0x3D | assumed |
| INMP441 mic | WS 4, SCK 5, SD 6 | assumed (standard Xiaozhi breadboard) |
| MAX98357A speaker | DIN 7, BCLK 15, LRC 16 | assumed (standard Xiaozhi breadboard) |
| Buttons | boot 0, talk 47, volume up 40, volume down 39 | assumed (standard Xiaozhi breadboard) |

Edit `main/boards/linc-robot-v1/config.h` for anything that differs. Points to check:
- **Emotion TFT:** the module has no CS pin, so the code uses SPI mode 3 and `TFT_CS_PIN = GPIO_NUM_NC` (a module with a CS pin needs mode 0 and that pin set). If the picture is shifted, mirrored or has inverted colours, edit `TFT_MIRROR_*`, `TFT_SWAP_XY`, `TFT_INVERT_COLOR`, `TFT_OFFSET_*` in `config.h`. The backlight is PWM-dimmed (LEDC timer 0) and restores the saved brightness at start; `self.screen.set_brightness` works.
- **I2C addresses (song OLED, and the alternative emotion OLED):** both OLEDs would share the bus, so they need different addresses. Modules of the same kind usually both answer at 0x3C; move one to 0x3D with the address jumper / solder pad on its back, then set `SONG_OLED_ADDR` / `EMOTION_OLED_ADDR` to match. The boot log prints every I2C address found.
- **Button on GPIO47 / GPIOs 39-40** and the audio pins are assumptions. If you wired buttons elsewhere, edit them.
- GPIO46 is a strapping pin (read only at reset), which is fine for an LED. GPIO48 is a plain LED here, not the WS2812 of some dev kits. Both are the turn signals (see "Turn signals").
- DRV8833 has no enable pins: speed is 20 kHz PWM on the IN pins (LEDC timer 3, channels 4-7). If a wheel turns the wrong way, swap its two pins or set `MOTOR_LEFT_INVERT` / `MOTOR_RIGHT_INVERT`. Make sure the DRV8833 nSLEEP pin is high.

## Two screens

- **Emotion screen** (the two-eye face): by default the **1.3" colour TFT** (`EyesLcdDisplay`, an `SpiLcdDisplay` subclass on SPI3). Set `EMOTION_DISPLAY_TFT` to 0 in `config.h` to use a monochrome 1.3" OLED instead (`EyesOledDisplay`). Both draw the same face through `EyesFace`, 27 expressions plus aliases, chosen by the emotion name the server sends (or from the web page / `self.robot.set_emotion`):

  | Group | Expressions and how they look |
  |---|---|
  | Calm | `neutral` round eyes; `relaxed` half-closed (flat lid); `cool` narrow with a flat lid; `bored` thin slits; `confident` one eyebrow down (smirk) |
  | Joy | `happy` smiling arches; `laughing` `> <`; `funny` one eye shut, one big; `silly` eyes of different sizes; `loving` pixel-art hearts; `embarrassed` arches looking down; `winking` right eye shut; `kissy` left eye shut; `delicious` cheeks pushed up; `excited` sparkles (+) |
  | Sad / fear | `sad` lids falling outward; `crying` sad with tears running down; `worried` small eyes, steep lids; `scared` big eyes, lids and tiny pupils; `angry` lids slanted to the nose |
  | Surprise | `surprised` big round eyes; `shocked` bigger eyes with tiny pupils |
  | Sleep / thought | `sleepy` slits; `sleeping` closed bowls; `thinking` glance up-right with one lid; `confused` uneven eyes; `dizzy` X eyes |

  Aliases also work: smile, joy, love, heart, shy, wink, yummy, cry, mad, wow, sleep, think, calm, tired, proud, dead. An unknown name shows the neutral face.

  On the **TFT** the face is in colour: cyan eyes, **red** when angry, **blue** when sad / crying / worried / scared, a **dim** teal when sleepy / bored / sleeping, pink hearts, yellow sparkles, light-blue tears. Layout: stock top bar (network, mute, battery, notifications) on a dark theme, the eyes in the middle (the 128x32 design scaled 2.2x), one scrolling chat line below. The face covers the stock emoji and chat area, which stays untouched underneath. On the monochrome OLED every colour is the text colour and the lids use the background colour, so it does not depend on the panel polarity; only the 128x64 layout has the face there.

  The eyes blink every 3-6 s and glance around every 6-10 s while the face is plain neutral. The shapes are an original design: the vendor page does not describe its eyes, so they will not look the same.
- **0.96" song OLED** (10 frames per second, which also keeps the I2C bus free when an emotion OLED shares it): `SongDisplay`, its own driver without LVGL. Idle: robot name + web address. Playing: "DANG PHAT", the title (scrolls when longer than 21 characters) and a 16-band spectrum computed with a 256-point FFT of the PCM that is sent to the speaker. The built-in 5x7 font has no accents, so Vietnamese titles are shown without diacritics (e.g. "Bai ca" for "Bài ca"). Drawing assumes the esp_lcd SSD1306 driver takes page-ordered 1-bit data; if the picture looks scrambled, that is the first thing to check.

## Turn signals

The two LEDs are the robot's turn signals (`TurnSignals`, `signal_pattern.*`). Three layers decide what blinks; the highest active one wins:

| Layer | When | What |
|---|---|---|
| 1. Command | `self.robot.lights`, or the "Đèn xi nhan" buttons on the web page | any pattern for 0.5-30 s; `off` cancels it and a running light show |
| 2. Movement | while the wheels turn | **turn left: left light blinks; turn right: right light blinks; forward or backward: both blink**; goes off when the robot has stopped (not when it starts slowing down) |
| 3. Face effect | when a face is shown | a short light show, never overrides the movement signals |

Blink: 250 ms on / 250 ms off (2 Hz); fast 100 ms, slow 500 ms; "alternate" = left and right take turns. Face effects: happy / laughing / funny / silly / excited / dizzy = alternating (3 s); loving / kissy / shy / winking / delicious = slow pulse (3 s); angry = fast flashes (3 s); surprised / shocked / scared = a few quick flashes (0.6 s); sad / crying / worried = slow blink (2 s); calm faces = none, and a calm face ends the previous show. A dance blinks left and right with its spins, with the face effects underneath.

Which side is which: GPIO46 = left, GPIO48 = right. Swap `LEFT_SIGNAL_GPIO` / `RIGHT_SIGNAL_GPIO` in `config.h` if your lights are the other way round; `SIGNAL_ACTIVE_LOW` is for LEDs that light with the pin low; `LIGHT_EFFECTS_ENABLED` 0 keeps only the movement signals and the commands. The face effects need the two-eye face (`EyesFace`): without a screen there is no emotion event to react to. The old state LED and activity LED were replaced by the turn signals (the song OLED shows what is playing). The 50 ms timer runs only while something blinks.

## Radio behavior

- Playback starts only when the device is idle. A voice command arrives mid-conversation, so the player pre-buffers, waits for the reply to be spoken, and if the device then sits in Listening for 2.5 s it closes the session with `Application::CloseConversation()`, a new call that only ever closes (the old `ToggleChatState()` would have started a chat if the robot had just gone idle).
- It stops by itself when the device leaves Idle (wake word, button, server message).
- The microphone and wake-word engine keep running during playback, so loud radio can trigger the wake word or be heard by the AFE. Lower the volume or use a board with AEC.
- Preset URLs come from the public radio-browser.info directory and may change. Pass any other URL to `self.radio.play` when one stops working.
- Not supported: fMP4 HLS segments, DRM, ICY metadata.
- While a song plays, the song OLED shows the title and the spectrum.

## Setup page and servers (agents)

The Wi-Fi setup page (hotspot, `192.168.4.1`) now follows the vendor's step-by-step guide:
- Networks show a signal word (strong / medium / weak) next to the dBm value, and a network that appears several times is listed once with its strongest signal.
- A reminder to choose a 2.4 GHz network, a reminder about typing the password exactly, and a warning when the password starts or ends with a space.
- After saving: "Đã lưu cấu hình" and "Thiết bị sắp khởi động lại và kết nối mạng" (English and Chinese too). The page then asks the robot to leave setup mode so it connects.
- New **Server** tab: the built-in default server plus up to 8 saved servers (name + OTA URL). "Use" makes one active, "Add" saves a new one. The same list is on the robot web page (section "Máy chủ / Agent"); choosing another server there reboots the robot to apply it.

What a "server" means: the robot asks the selected OTA URL where to connect (WebSocket or MQTT) and which activation code to show. **The cloud "agents"** (name, language, voice, role) are not configured on the robot: they live on that server's own dashboard, and the robot is attached to one by its activation code. So the robot can choose WHICH server to talk to, not which agent inside it.

Changing the server on the setup page asks for the **robot key** (the admin key above): the setup hotspot is open, so otherwise anyone nearby could change it. Look at the song OLED for the key. A custom server can also answer the firmware check with an update, so only add servers you trust. Code: `local_components/esp-wifi-connect/server_profiles.*`; NVS namespace `wifi`, keys `ota_url` (active) and `srv_profiles`.

## Free radio and songs, with automatic switching

**Radio directory** (`main/radio/radio_directory.*`): searches [radio-browser.info](https://www.radio-browser.info) (free, no key, many Vietnamese stations) by name, country (default `VN`) and genre. Only MP3 / AAC / HLS stations are kept because that is what `RadioPlayer` decodes. Say "tìm đài nhạc vàng" or use the web page (search box under Radio).

**Switching** (`RadioPlayer::PlayList()` / `Next()`): the player gets a list of candidates and plays the first one that works.
- Moves to the next candidate when an entry cannot be opened, does not start within about 25 s, or (live radio) ends by itself.
- `self.media.control` with `next` / the "Doi dai / doi bai" button skips to the next one on request.
- It never switches on its own when you interrupt it (wake word, button) or when the robot stays busy: it just stops.
- A track from `self.music.play` that plays to its end is a normal finish; only a track that fails is replaced.
- The display shows "Doi sang: <name>" on a switch and "Khong phat duoc, thu lai sau" when nothing in the list works.

**Songs** (`MusicClient`): in this order, collecting every result as a candidate: your music server (if set), then Jamendo (if a client id is set). Get a free client id from developer.jamendo.com, enter it on the robot web page ("Nguon nhac mien phi"). Jamendo is independent music under Creative Commons licences: little or no mainstream Vietnamese pop. Check Jamendo's terms for your use (commercial use needs a separate licence).

Not verified from here: the radio-browser mirror hostnames other than `de1`, the exact Jamendo answer fields, and whether the HTTP client follows redirects on the Jamendo links. If a source misbehaves, the log tells which; the switching mechanism then simply moves on.

## Ported from the Vietnam fork

Source: `xiaozhi-esp32_vietnam-develop_vn` (version 2.0.5.06, a community fork of xiaozhi-esp32). It is older than this repo (2.5.3) and its features are woven into its own `Application`, so code was **re-implemented against the current architecture**, not copied.

| Feature of the fork | Result here |
|---|---|
| Music search server (`esp32_music.cc`) | **Ported** as `main/radio/music_client.*`: same HTTP contract (`/stream_pcm`, auth headers, URL rules). No built-in server: the fork's default points to a third-party host, and that host would receive the device MAC address and your song requests, so you choose it (web page, NVS `wifi/music_url`) |
| Radio stations and Vietnamese name matching (`esp32_radio.cc`) | **Ported** into `RadioPlayer::Channels()` / `Find()` |
| Firmware upload from a browser (`ota_server.cc`, port 80, no authentication) | **Ported, re-written** as `main/ota_upload.*` plus a card on the robot web page. The browser sends the raw `.bin`, so there is no multipart parser. It is protected by an 8-hex-character key generated on first use and shown only on the robot (display notification at start, the song OLED when idle, and the log); the image must pass `esp_ota_end()` and be built for the same project name; only allowed while the robot is idle |
| Spectrum analyzer / music visualizer | Not ported: this repo already has `SongDisplay` for the song OLED (see above) |
| Lyrics (`lyric_manager`) | Not ported (the song OLED is 21 characters wide and ASCII-only) |
| Weather standby screen, QR code, MP4 / video player, SD-card music and video, touch driver, `device_state_event` | Not ported: they need an LCD, an SD card or the fork's `Application` internals, and the robot has OLED screens and no SD card |
| Acoustic (AFSK) Wi-Fi setup (`afsk_demod`, `sonic_wifi_config.html`) | Not ported: depends on the old audio pipeline |
| Assets upload from the OTA page | Not ported: the assets code changed in 2.5.x |
| Partition tables `16m_logo.csv`, `16m_otto_robot.csv` | Not ported (5 MB app slots; not needed for this robot) |
| Five Vietnamese community boards (`xiaozhi-ai-iot-vietnam-*`, LCD boards, one with ML307) | Not ported: LCD / SD hardware, written against the older display and SD code |
| `.github` Copilot instructions, issue templates, `docs/av_render_architecture.md` | Not ported |

Web page additions: "Nhac theo ten bai" (song, artist, music server address) and "Cap nhat firmware tu file" (OTA key, `.bin`, progress bar). New endpoints: `POST /api/music`, `/api/music_server`, `/api/ota_upload`.

Known limits: the fork's VOV URLs and the music server API could not be tested from here; the music server is a third-party service whose terms and availability are outside this project; lyrics are not shown.

## Per-board settings, soft start, tests

- **Only this board gets the heavy options.** `main/boards/linc-robot-v1/config.json` appends `CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=y` (802.1X costs flash and RAM) and `CONFIG_WIFI_CONNECT_PORTAL_CHECK=y` (captive-portal probe after every connect, off by default in `local_components/esp-wifi-connect/Kconfig`). `sdkconfig.defaults` keeps enterprise support off for every other board. NVS `wifi/portal_check` (0 / 1) overrides the build default at run time.
- **802.1X outer identity.** Many schools want `anonymous@school.edu.vn` as the outer identity: set it in the setup page, Advanced tab (NVS `wifi/eap_identity`). Empty means the username is used. The server certificate is still not validated.
- **Soft start.** `RobotController` ramps wheel speed 12 % every 20 ms (full speed in about 170 ms, a reversal passes through zero). The timeout at the end of a move also slows down; `self.robot.stop` / the web stop button cut the motors at once.
- **Shared code and tests.** The plain-C++ parts have no ESP-IDF dependency and are unit-tested on a PC by `scripts/tests/test_linc_robot_logic.py` (URL coding, HLS playlist parsing, channel-name matching, accent folding): `radio/url_util.*`, `radio/hls_playlist.*`, `radio/radio_channels.*`, `boards/linc-robot-v1/ascii_fold.*`. The test skips itself when no host C++ compiler is installed, and it has **not been run yet** (no compiler on the machine used to write it).

## Build

`idf.py menuconfig` -> Xiaozhi Assistant -> Target Board -> `Linc Robot V1`, or `python scripts/build.py linc-robot-v1`.

Prefer a window? Run `scripts\builder_gui.bat` (or `python scripts/builder_gui.py`): pick the board, build, flash and monitor
from one screen. See [builder.md](builder.md).

## Open risks

- **Not compiled.** Check first: the `esp_audio_simple_dec_*` and `esp_audio_dec_register_default()` names in esp_audio_codec 2.5 (`radio_player.cc`), `xStreamBufferCreateWithCaps` availability, and the `ToolResult` returns in `robot_controller.cc`.
- Radio and music over HLS need PSRAM for the 96 KB buffer (falls back to 32 KB internal RAM).
- `Ota` is constructed inside the web handler and blocks the single web server task during the check.
- The setup hotspot is open: the robot key protects server changes there, but not the Wi-Fi password form itself.
- Not done: validating the 802.1X server certificate; fMP4 HLS; ICY (song title) metadata; an mDNS name such as `robot.local` (it needs a new component dependency); extracting the captive-portal form parser into a unit-tested unit.
- `local_components/esp-wifi-connect` is a local copy: merging a newer upstream release needs a manual compare (see its `NOTICE.md`).
