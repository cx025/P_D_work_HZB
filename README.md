# ESP32-C3 haptic and audio controller

The integrated firmware is split into independent modules:

- `main/pressure_sensor.c` reads and debounces the active-low pressure switch.
- `main/drv8833.c` drives the vibration motor through the DRV8833 using PWM
  on GPIO4 and GPIO5, with GPIO1 as the driver's enable/standby input.
- `main/speaker.c` powers on the NS4168 using its CTRL pin and outputs test
  tones and decoded MP3 audio directly over I2S.
- `main/main.c` coordinates the modules and provides serial commands.

The current build runs the sitting-reminder controller
(`main/pressure_test.c`). The serial console prints `0` when pressure is
detected and `1` when there is no pressure. The speaker plays the soft clip
once after startup. Pressure alone does not trigger playback: the seat must
remain occupied for 30 seconds to start a session, then cumulative seated-time
thresholds trigger the soft, blast, and awkward clips. The third reminder
repeats every 60 minutes until an effective leave. Time away is excluded from
the seated duration, and a leave must last more than one minute to finish the
session. The active-LOW sensor input uses 30 ms debounce. BLE mute prevents
session reminders while muted. After a 3 s power-stabilization delay, the
motor gives a 500 ms startup pulse; the soft clip also plays at startup.
Sitting time starts after the pressure sensor
has confirmed occupancy for 30 seconds. The three Mini Program reminder
thresholds then trigger the soft clip, the blast clip with a 2 s first-level
vibration, and the awkward clip with a second-level vibration. The Mini Program
independently controls each reminder's motor amplitude and vibration frequency,
plus the PWM carrier frequency (5–30 kHz).

## Wiring

- Pressure switch: GPIO0 and GND; active LOW by default.
- DRV8833: enable/standby on GPIO1, AIN1 on GPIO4, and AIN2 on GPIO5.
- NS4168: CTRL on GPIO3, LRCLK/WS on GPIO7, BCLK on GPIO6, and SDATA on
  GPIO10. Connect the speaker to the NS4168 output. Do not connect either
  bridged speaker output to ground.

The NS4168 accepts I2S digital audio directly; no external DAC is needed.
Firmware holds CTRL low for at least 100 us, then raises it and waits for the
amplifier to start before enabling I2S playback.

The three MP3 files are embedded in the application image in flash and decoded
frame-by-frame without loading an entire decoded clip into RAM. Stereo MP3 is
mixed to mono and copied to both I2S slots for the NS4168. Volume is adjustable
from 0 to 100 percent using software scaling and 16-bit clipping.
Reminder settings, mute state, and daily aggregates persist in NVS. The phone
synchronizes its local date over BLE when the Mini Program connects; the ESP32
does not need Wi-Fi or an RTC. For Android and iPhone, import `miniprogram/` into
WeChat DevTools and follow its README to use the native WeChat BLE APIs; a
registered WeChat Mini Program AppID is required for phone preview and release.
The firmware advertises the BLE control service UUID in its scan response so
the Mini Program can identify the ESP32 even when iOS does not expose its name;
reflash the firmware after updating this advertising configuration.
The Web Bluetooth control page in `web/` remains available for compatible
browsers.
Reminder settings, mute state, and daily aggregates persist in NVS.
The custom 4 MB flash layout reserves a 704 KB `storage` LittleFS partition for
the offline usage log; the factory app keeps the remaining 3.25 MB after NVS,
PHY, and storage.

## Offline usage log and BLE sync

The device keeps a per-use history independently of the phone. Each session is
recorded as a start record followed by an end record carrying the actual seated
duration, the reminder count, and whether the user responded. Every reminder
that actually plays is logged with its level, and a power cut or reset while a
session is open is logged as an abnormal interruption with the reset reason and
the partial seated duration. Records are appended to a LittleFS log with a
per-record CRC32 and monotonic sequence number, so a torn write from a power
cut is detected and dropped on the next boot without corrupting earlier
records. The log is compacted once synced, so it never fills up.

When the Mini Program reconnects, it sends `SYNC <after_seq>`; the ESP32
streams every newer record over a dedicated history characteristic and finishes
with a `done` marker. The Mini Program stores the records locally, sends
`ACK <seq>`, and the device persists the cursor so the next sync only sends new
records. The `CLEAR` command erases every record on the device and resets the
sync cursor; the Mini Program's "清空" button also drops its local copy.

## Build and test

Open this folder in VS Code with the ESP-IDF extension, select target
`esp32c3`, then build and flash the project. Open the configured serial monitor
at 115200 baud to view pressure state values and audio playback logs. Open
`web/index.html` through HTTPS, or serve it from `localhost`, in a recent
Chrome or Edge browser. Connect to `ESP32C3-Haptic`, change the settings, and
press **保存设置到设备**. The phone or computer must be within BLE range;
OS-level pairing is not required. A `localhost` URL works only on the computer
running the web server; phone access requires a trusted HTTPS host. The three
thresholds are in minutes, from 1 to 1440, and must be non-decreasing.

For the WeChat Mini Program, import `miniprogram/` into WeChat DevTools, replace
`touristappid` in `miniprogram/project.config.json` with your registered AppID,
then compile and preview on an Android phone or iPhone. Keep the phone near the
ESP32-C3 and grant WeChat Bluetooth access when prompted. See
`miniprogram/README.md` for platform-specific setup notes.
