# Firmware contract: what the website expects from AIPI's Muse build

The website works against AIPI's fork of `facebookincubator/muse-gadget-sdk`, built with the
AIPI profile:

```sh
cd esp32
tools/muse/board.sh build aipi        # → build-muse-aipi/
```

Meta's build compiles the SDK token in (`CONFIG_GADGET_SDK_TOKEN`). A token is personal to its
owner and limited to a number of devices (Gadget SDK Terms), so AIPI must not ship one token to
every customer. Each owner brings their own token, and the site writes it to their device. That
needs the changes below. File and function names refer to the SDK as of commit `812c46f`.

## 1. SDK token from NVS, set over USB (required)

**Read it.** `main/identity.c` › `identity_sdk_token()` returns `CONFIG_GADGET_SDK_TOKEN`.
Change it to return the NVS key `sdk_token` when the compiled-in value is empty. Load it once in
`identity_init()`: `config_store_init()` already runs before it in `app.c`, and
`link_pairing_init()` / `vm_api_set_sdk_token()` read it right after.

**Keep it on reset.** `config_clear_setup()` and `config_clear_pairing()` erase fixed key lists
in `main/config_store.c`. Leave `sdk_token` out of both, so Reset pairing keeps the token.

**Set it.** Add a console command. Serial lines starting with `>` go to `console_command()` in
`components/muse/muse_input.c`, then to `muse_ble_command()`:

```
>sdk.token=mgst_<43 chars>
```

- Validate it like `cmake/validate_config.cmake`: 48 characters, `^mgst_[A-Za-z0-9_-]*[AEIMQUYcgkosw048]$`.
- Save to NVS (`config_set_str("sdk_token", …)`).
- Reply on its own line, then restart so pairing picks it up:
  - `@sdk.token ok restart` then `esp_restart()` after ~300 ms
  - `@sdk.token error invalid` or `@sdk.token error storage`
- Never echo the token in logs; print the first 12 characters at most, as the SDK already does.

Do **not** add this command to the BLE "phone setup" characteristic. USB only.

## 2. Status fields (required)

`>status` already prints `@status {"board":"AIPI Lite","chat":…,"device":{…}}` from
`muse_ble_status_json()`. The site reads `device.name` (`MuseGadget-XXXXXX`), `device.fw` and
`device.link.state`. Please add:

- `"sdk_token": true|false` inside `device`.
- Set `PROJECT_VER` for release builds (`idf.py -DPROJECT_VER=1.0.0 build`) so `device.fw` is
  not `999.0.0`. Use the same value as `version` in the website manifest.

`link.state` strings the site understands (from `muse_link_state_name()`): `Starting`,
`Ready to pair`, `App connected`, `Confirm pairing`, `Connecting`, `Online`, `Offline`, `Error`.
Keep them unchanged, or update `linkMode()` in `assets/js/flasher.js`.

## 3. Voice (TTS) setup over USB (optional, added 2026-10-13)

The site shows an optional "Make it talk" step. It configures the TTS service over the same
USB console, with the same reply discipline as `>sdk.token=`. Keys go to NVS, one per field:

| Console command | NVS key | Max length | Effect |
|---|---|---|---|
| `>tts.url=<https://…>` | `tts_url` | 255 | TTS HTTP endpoint |
| `>tts.key=<api key>` | `tts_key` | 255 | Authorization key (Bearer) |
| `>tts.model=<model>` | `tts_model` | 63 | Model name |
| `>tts.voice=<voice>` | `tts_voice` | 63 | Voice id |
| `>tts.clear` | (erase all four) | — | Back to text-only mode |
| `>tts?` | — | — | Report state |

- Reply on its own line per command: `@tts.url ok` / `@tts.url error invalid` /
  `@tts.url error storage`. **No restart** — the site sends the four commands back to back,
  then one `>tts?` to confirm.
- `>tts?` replies `@tts {"set":true|false,"url":"…","model":"…","voice":"…"}`. Never echo the key.
- Treat TTS as configured only when `tts_url` is non-empty (key/model/voice may be empty for
  services that don't need them).
- **Read order in the firmware**: NVS value first; when empty, fall back to the compiled-in
  `STEP_TTS_*` macros in `muse_chat_session.cpp` (internal test builds pre-fill them with a real
  service; release builds ship them as placeholders). When both are empty, or the request fails,
  keep the existing text-only fallback.
- Leave `tts_*` out of `config_clear_setup()` / `config_clear_pairing()` too (same as the token).
- `>status` gets `"tts": true|false` inside `device`, next to `sdk_token`.

## 4. Screens on the 128 px display (required for the guide to match)

`components/muse/muse_ui.c`. Meta's build hides the gadget name on the AIPI's square 128 px screen.

| State | Show |
|---|---|
| `Ready to pair` | "In the Muse app, pick" / "MuseGadget-" / the six characters large / "Ready to pair" |
| `Confirm pairing` | "Press" / "the right button" / a countdown from 60 s / an arrow toward the right button |
| `Connecting` | "Joining Wi-Fi…" / "then connecting to Muse" |
| `Offline` or `Error` | QR code for `https://aipi.com/m/<CODE>` / "Can't reach Muse" / "Scan for help" |

LVGL 9.5 is already a dependency; enable `CONFIG_LV_USE_QRCODE` for the QR. Keep the URL short
(uppercase helps: `HTTPS://AIPI.COM/M/A1B2C3` fits QR version 2).

Keep the BLE name prefix `MuseGadget`; the Muse app and Meta's docs look for it.

## 5. Avatar (required before distributing binaries)

The Apache licence doesn't cover the Jollybot avatar: `esp32/avatar/muse_pixel.c`,
`esp32/avatar/happy_anim.c/.h` (built from `jollybot.gif`, listed in `main/CMakeLists.txt`).
Replace them with AIPI's own character before publishing builds; `tools/muse/AVATAR_RECIPE.md`
shows how the renderer is structured.

## 6. Nice to have

- `>sdk.token?` → `@sdk.token set|unset` for support.
- Turn on NVS encryption (`CONFIG_HOMEHUB_NVS_ENCRYPTION`) if it fits the AIPI's eFuse plan.
- OTA: the AIPI profile enables OTA. Decide whether AIPI serves updates; until then, the site is
  the update path.

## Test checklist (real hardware)

1. Fresh AIPI on the AIPI system → setup.html: connect shows ESP32-S3, 16 MB, the code.
2. Backup file is exactly 16,777,216 bytes and restores with restore.html.
3. After install the site reconnects to the console within 25 s and gets `@status`.
4. `>sdk.token=` reply arrives within 6 s; after restart `@status` shows `"sdk_token":true`.
5. Pairing in the Muse app moves `link.state` through Ready to pair → App connected →
   Confirm pairing → Connecting → Online, and the site ticks each step.
6. Reset pairing on the device keeps the token (`"sdk_token":true`), forgets Wi-Fi.
7. Windows 10/11, macOS and Linux (Chrome and Edge).
