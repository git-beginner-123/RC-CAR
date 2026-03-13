RC_CAR

Independent ESP-IDF project for the car side.

Current framework:
- Open Wi-Fi AP: `RC_CAR`
- HTTP control endpoint: `GET /api/drive?cmd=forward|backward|left|right|stop`
- JPEG frame endpoint: `GET /capture`
- MJPEG stream endpoint: `GET /stream`

Files to adjust for hardware:
- `main/rc_car_config.h`
  Fill motor GPIO pins before enabling real drive output.

Camera status:
- The camera path was reduced to a minimal JPEG-only web interface.
- Default profile is `OV2640 JPEG 320x240`.
- Current design goal is a stable `/capture` and `/stream`, not a custom
  diagnostic web UI.

Suggested build flow:
- `idf.py set-target esp32s3`
- `idf.py build`
