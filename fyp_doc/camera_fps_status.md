# Camera FPS Probe Status

Date: 2026-05-20

## Question

Can the KV260 AR1335/AP1302 camera path be driven above the current
approximately 30 fps cadence, for example at 60 fps?

Working hypothesis: the AP1302 output is only `128x128`, but the sensor/ISP
input side is still `4208x3120`, so the camera may be limited by full-sensor
readout/processing bandwidth rather than the small output frame size.

## Current Evidence Before Probe

- `/dev/video0` reports `NV12`, `128x128`.
- `/dev/video0` does not support `VIDIOC_G_PARM`.
- `/dev/video0` does not enumerate frame sizes or frame intervals.
- The AR1335/AP1302 media graph reports:
  - AR1335 source: `SGRBG10_1X10/4208x3120`
  - AP1302 source: `VYYUYY8_1X24/128x128`
  - CSI-2 RX sink/source: `VYYUYY8_1X24/128x128`
- The current run scripts set the AP1302/CSI pad format with `media-ctl`.
  They set output resolution, not frame interval.
- Previous bring-up notes show `mediasrcbin` can negotiate the AP1302 graph
  more completely than plain `v4l2-ctl`/`v4l2src`.

## Probe Added

New script:

```text
vision_software/visionsoc_main/camera_fps_probe.sh
```

It stops `visionsoc_main`, prints V4L2/media state, tries the normal
frame-rate ioctls non-fatally, then captures short `mediasrcbin` streams at:

```text
128x128 NV12 framerate=30/1
128x128 NV12 framerate=60/1
```

Actual fps is computed from:

```text
captured_bytes / 24576 bytes_per_frame / elapsed_time
```

AP1302 `v4l2-dbg --log-status` is sampled when available so that userspace
capture rate can be compared with AP1302 internal frame counters.

## Results

Run on KV260 with:

```sh
cd /home/ubuntu/vision_software/visionsoc_main
CAPTURE_SECONDS=6 FPS_LIST='60 30' ./camera_fps_probe.sh
```

The script was updated to reapply the VisionSoC overlay before each capture,
because a stale `v4l2-ctl --stream-mmap` process initially held `/dev/video0`
busy and later left the camera path in a bad streaming state.

Frame-rate API probes:

```text
v4l2-ctl -d /dev/v4l-subdev1 --set-subdev-fps pad=2,fps=60
  VIDIOC_SUBDEV_S_FRAME_INTERVAL: failed: Inappropriate ioctl for device

v4l2-ctl -d /dev/v4l-subdev1 --get-subdev-fps 2
  VIDIOC_SUBDEV_G_FRAME_INTERVAL: failed: Inappropriate ioctl for device

v4l2-ctl -d /dev/video0 --set-parm=60
  VIDIOC_S_PARM: failed: Inappropriate ioctl for device

v4l2-ctl -d /dev/video0 --get-parm
  VIDIOC_G_PARM: failed: Inappropriate ioctl for device
```

Measured `mediasrcbin` captures with fresh overlay reset before each row:

| Requested Caps | Bytes | Frames | Elapsed | Actual FPS |
|---|---:|---:|---:|---:|
| `128x128 NV12 framerate=60/1` | `3686400` | `150` | `6.292085 s` | `23.84` |
| `128x128 NV12 framerate=30/1` | `3661824` | `149` | `6.291465 s` | `23.68` |

AP1302 status counters advanced during both captures, but the delivered frame
count stayed around 24 fps in both cases. Asking GStreamer for `60/1` did not
increase the actual userspace frame rate.

## Conclusion

The current software stack has no exposed V4L2 frame-interval control, and
`mediasrcbin` caps negotiation does not make the camera deliver 60 fps frames.
This supports the working hypothesis: the AP1302 output is downscaled to
`128x128`, but the active sensor/ISP mode is still constrained by the
AR1335/AP1302 capture path, not by the small output frame size.

To get true 60 fps, the next work would need to be AP1302/AR1335 mode work:
selecting or forcing a lower-resolution/windowed/binned sensor input mode and
making the driver/firmware expose that frame interval. Changing only
`media-ctl` output size or GStreamer caps is not enough.

## DTS Clock Experiment

The active `system_top_wrapper.dts` had an AP1302 fixed-clock value of
`0x48000000` with a suspicious comment. `system_top_camtest4.dts` uses the
real IAS crystal value, `24000000`.

Added an isolated DTS variant:

```text
fpga/dts/system_top_wrapper_camfps24.dts
fpga/dts/system_top_wrapper_camfps24.dtbo
```

Only functional DTS change versus `system_top_wrapper.dts`:

```dts
ap1302_clk: sensor_clk {
    #clock-cells = <0>;
    compatible = "fixed-clock";
    clock-frequency = <24000000>;
};
```

The dtbo was compiled on the KV260 because local `dtc` is not installed:

```sh
dtc -@ -I dts -O dtb \
  -o /tmp/system_top_wrapper_camfps24.dtbo \
  /tmp/system_top_wrapper_camfps24.dts
```

Probe command:

```sh
cd /home/ubuntu/vision_software/visionsoc_main
CAPTURE_SECONDS=6 FPS_LIST='60 30' \
  DTBO=/lib/firmware/xilinx/visionsoc/system_top_wrapper_camfps24.dtbo \
  ./camera_fps_probe.sh
```

Measured with fresh overlay reset before each row:

| DTS | Requested Caps | Bytes | Frames | Elapsed | Actual FPS |
|---|---|---:|---:|---:|---:|
| `camfps24` | `128x128 NV12 framerate=60/1` | `3710976` | `151` | `6.293465 s` | `23.99` |
| `camfps24` | `128x128 NV12 framerate=30/1` | `3710976` | `151` | `6.290692 s` | `24.00` |

Result: fixing the AP1302 fixed-clock DTS property did not change delivered
frame rate. This is consistent with the AP1302 driver treating the clock as
enable/probe bookkeeping rather than using its rate to program sensor timing.

The board was restored to the canonical `system_top_wrapper.dtbo` after the
experiment, and `visionsoc_main` was restarted.

## Sensor-Input Mode Investigation

Follow-up investigation used the experimental `camfps24` dtbo only. The
original `system_top_wrapper.dts` was not edited.

After loading `system_top_wrapper_camfps24.dtbo`, the raw sensor side still
enumerates as full AR1335 resolution:

```text
ar1335 0 pad0:
  SGRBG10_1X10/4208x3120

ap1302.4-003c pad0 sink:
  SGRBG10_1X10/4208x3120

ap1302.4-003c pad2 source after media-ctl:
  VYYUYY8_1X24/128x128
```

Runtime attempts to set the raw sensor/AP1302 sink side to smaller formats
were accepted by `media-ctl` syntactically but coerced back to full resolution:

```sh
media-ctl -d /dev/media0 \
  -V '"ar1335 0":0 [fmt:SGRBG10_1X10/1920x1080 field:none colorspace:srgb]'
media-ctl -d /dev/media0 \
  -V '"ap1302.4-003c":0 [fmt:SGRBG10_1X10/1920x1080 field:none colorspace:srgb]'

media-ctl -d /dev/media0 \
  -V '"ar1335 0":0 [fmt:SGRBG10_1X10/128x128 field:none colorspace:srgb]'
media-ctl -d /dev/media0 \
  -V '"ap1302.4-003c":0 [fmt:SGRBG10_1X10/128x128 field:none colorspace:srgb]'
```

In both cases `media-ctl -p` still reported:

```text
ar1335 0 pad0:             SGRBG10_1X10/4208x3120
ap1302.4-003c pad0 sink:   SGRBG10_1X10/4208x3120
```

Subdevice frame-size enumeration confirms the same thing:

| Device / Pad | Code | Enumerated Size Range |
|---|---|---|
| `/dev/v4l-subdev0` pad 0, AR1335 source | `0x300a` / `SGRBG10_1X10` | `4208x3120 - 4208x3120` |
| `/dev/v4l-subdev1` pad 0, AP1302 sink | `0x300a` / `SGRBG10_1X10` | `4208x3120 - 4208x3120` |
| `/dev/v4l-subdev1` pad 1, AP1302 second sink | `0x300a` / `SGRBG10_1X10` | `4208x3120 - 4208x3120` |
| `/dev/v4l-subdev1` pad 2, AP1302 source | `0x2100` / `VYYUYY8_1X24` | `24x16 - 4224x4092` |

Selection/crop queries also show full sensor bounds/native size on AP1302:

```text
crop_bounds: 4208x3120
native_size: 4208x3120
crop:        4208x3120
```

This means the currently loaded Xilinx AP1302/AR1335 driver exposes only one
raw sensor input mode: full `4208x3120`. The AP1302 output can be scaled down
to `128x128`, but that is downstream of the full-resolution sensor/ISP input.

This matches the upstream AP1302 driver code pattern. In the driver patch, the
AR1335 sensor info entry is:

```c
.model = "onnn,ar1335",
.resolution = { 4208, 3120 },
.format = MEDIA_BUS_FMT_SGRBG10_1X10,
```

The same driver enumerates fixed sink-pad sizes from `sensor_info->resolution`
and only allows free scaling on the AP1302 source pad. It also has no
`frame_interval` operation, matching the board's `VIDIOC_*FRAME_INTERVAL`
failures.

Source checked:

```text
https://patchew.org/linux/20250623-ap1302-v3-0-c9ca5b791494%40nxp.com/20250623-ap1302-v3-2-c9ca5b791494%40nxp.com/
```

AMD/Xilinx's KV260 Smart Camera debugging docs show the same topology: AR1335
and AP1302 sink at `4208x3120`, with the AP1302/CSI output path separately
configured. That confirms this is the reference driver model, not just a
VisionSoC DTS mistake.

```text
https://xilinx.github.io/kria-apps-docs/kv260/2022.1/build/html/docs/smartcamera/docs/debug-sc.html
```

Current conclusion: the hypothesis is strongly supported. The deployed
driver/firmware path reads the AR1335 as full `4208x3120`; `128x128` is only
AP1302 output scaling. A DTS-only change cannot expose 60 fps unless the
driver supports a lower raw sensor mode or a different AP1302 firmware/mode
blob is used.

## AR1335 Driver Binding Check

The standalone Xilinx `ar1335.ko` module exists on the KV260 image:

```text
/lib/modules/5.15.0-1027-xilinx-zynqmp/kernel/drivers/media/i2c/ar1335.ko
```

Read-only module-string inspection shows that this standalone driver contains
multiple AR1335 mode names and frame-interval operations:

```text
mode_1280x720_60
mode_1920x1080_30
mode_3840x2160_30
ar1335_g_frame_interval
ar1335_s_frame_interval
```

However, that driver is not the one controlling the camera in the current
VisionSoC overlay. On the board:

```text
lsmod:
  ap1302 45056 1

i2c driver binding:
  /sys/bus/i2c/drivers/ap1302/4-003c

video subdevices:
  /dev/v4l-subdev0 -> .../4-003c/4-003c-ar1335.0  name: ar1335 0
  /dev/v4l-subdev1 -> .../4-003c                  name: ap1302.4-003c
```

So the `ar1335 0` media entity is an AP1302-created child subdevice, not an
independently bound `ar1335.ko` sensor device. This also matches the DTS:

```dts
ap1302: isp@3c {
    compatible = "onnn,ap1302";
    sensors {
        onnn,model = "onnn,ar1335";
        sensor@0 { ... };
    };
};
```

There is no separate I2C sensor node with `compatible = "ar1335"` for
`ar1335.ko` to bind to.

Implication: the existence of `mode_1280x720_60` inside `ar1335.ko` does not
help the current AP1302 pipeline directly. The deployed AP1302 driver owns the
sensor description and currently exposes the AR1335 input side as one fixed
`4208x3120` mode. A DTS-only experiment can adjust AP1302 node properties, but
it cannot make the AP1302 driver use the standalone AR1335 driver's 720p60
mode table.

Likely paths for true 60 fps are therefore:

1. Patch/rebuild the AP1302 driver so its internal AR1335 sensor-info path can
   select a lower input mode and program the AP1302/sensor accordingly.
2. Use a different AP1302 firmware/configuration blob that initializes the
   AR1335 for a lower-resolution high-fps mode.
3. Redesign the camera path to bind the AR1335 directly, bypassing AP1302.
   That is a much larger hardware/software change and would lose AP1302 ISP
   functions unless equivalent demosaic/colour processing is added elsewhere.
