# BONSAI — ECCV 2026 Demo Video Plan

**Purpose:** A ~3.5 min animated explainer for the ECCV 2026 conference, aimed at
vision researchers interested in *programmability* and *hardware architecture*.
**Spine:** "An image is one vector register" — we follow a single image on its
journey from the camera, into the 2D square register file, through one-instruction
whole-image compute, the free transpose, and out as a result.
**Style:** 3blue1brown-flavoured — clean pixel grids, integers overlaid on cells,
ops rippling/sweeping across the grid, smooth `Transform`s. Dark background,
restrained palette, on-screen captions (no audio).
**Tooling:** Manim Community v0.20.1 (installed). ffmpeg via imageio-ffmpeg.

---

## 0. Canonical numbers used on screen (keep consistent everywhere)

| Quantity                | Value shown                          |
|-------------------------|--------------------------------------|
| Image / register shape  | 128 × 128 px = **16,384 pixels**     |
| One image               | = **one logical 2D vector register** |
| Hardware rows (logical) | **128**, broadcast per instruction   |
| Element width           | int8 (SEW = 8)                       |
| Mode CSR                | `0x7c0` (0 = horizontal, 1 = vertical)|
| Live demo               | AMD Kria KV260, **30 FPS** HDMI      |

For pedagogy the on-screen grid is **rendered smaller** (e.g. 32×32) with a label
"128×128" so individual cells stay legible. The 16,384 number is the headline.

---

## 0b. On-die data path (from `fyp_diagram/selected/asic_die_v2.drawio`)

Two dies, with a hard chip boundary the story leans on:

```
NEAR-SENSOR PROCESSOR DIE                                   |  SoC DIE
[On-Die Image Sensor] -> [Frame Buffer] -> [DMA] -> [2D T1  |  [Off-Chip Scalar Core
                                            Vector Fabric]  |   (DDR / Host SoC)]
                                                 ^   |      |
                              Vector Instruction |   | Scalar Result Return
                                 (in)            |   v (out)
                                          [AMBA / AXI Interface] <-> High-Bandwidth + Indexed AXI
```

Key narrative facts this gives us:
- The **image sensor is on the same die** as the 2D fabric. Pixels go
  sensor → frame buffer → DMA → fabric **without leaving the chip**.
- Only **vector instructions come in** and **scalar results go out** across the
  die boundary to the off-chip scalar core. The frame itself stays resident.
- This is the literal picture behind "data stays on-die" (privacy/bandwidth).
  Use it directly in S2 (load) and S6 (why it matters).

---

## New Arc
So currently, I am typing on a Markdown file, and I'm trying to describe a scene breakdown for a video. Here's how it goes:

**Scene 1:** Show the title "Bonsai" with the researcher's name and a Bonsai tree logo.

**Scene 2:** The idea of the video is to explain computer vision from its very foundation. For example, we see an image, and then that image goes into the camera sensor, then to the architecture process, and is sent somewhere else. So, in this scene, I would like to show an image of a cat.

**Scene 3:** I would like to show how a conventional pipeline works.
3A. I'd like to have an image of a cat with a camera sensor on the right side of the video. Two lines will be drawn from the cat to the sensor, showing that the camera sensor is capturing the cat.
3B. The camera sensor shifts to the center of the display, indicating that our focus is shifting towards the sensor.
3C. This scene shows the entire pipeline of how traditional computer vision is handled. For example, the sensor captures the image and sends it to the CPU, which then sends the entire image to the GPU. Here, I would like to see a ripple effect showing the camera sensor, an arrow pointing to the CPU, and another arrow pointing to the GPU. This illustrates that the entire image is sent through each pipeline, and they are on different dies. Also, show that they use power by displaying an electricity logo.

**Scene 4:** I would like to show a similar concept to Scene 3, starting with the same cat image.
4A. On the right-hand side, show our new Neurosensor die. This die, displayed on the right-hand side screen, includes a camera sensor, a buffer, and our Bonsai processor, along with the interconnects we previously discussed in another document.
4B. Show the cat again with two lines drawn towards the camera sensor, indicating that this sensor is capturing the cat.

**Scene 5:** We zoom into the Bonsai.
5A. Show that the cat image is being captured by the sensor, loaded into the buffer, and then moved towards the Bonsai architecture.
5B. We shift the Bonsai architecture to the center of the frame and zoom in to see the clear architecture of the floor plan. It shows square register planes stacked together, overlaying each other. Below them, we have the compute lanes. The compute lane is connected to a load-store unit, which has arrows drawn from the outer to the inner parts of the architecture to show communication between the outer buffer and the on-chip architecture.

**Scene 8:** We zoom into those square register files. We break them down because they were overlayed. Now, we have an animation of them filling up the entire screen. We break them down into 32 2D square registers, as we have 32 stacked together. We won't show all 32 stacked, but multiple ones to convey that there are many. When we expand them in Scene 8 for the animation, we see 32 of them arranged on the screen.

**Scene 9:** We see the naming of each of these square registers from V0 up to V31 under each square register plane.

**Scene 10:** We collapse all of the square register planes back to the stacked overlay version, just as we showed the architecture previously, because we are about to shift into another subsystem in the architecture.

**Scene 11:** Before we zoom into the compute subsystem, with the stacked overlay square registers, we show that they are scalable in the horizontal and vertical directions. For scalability, I would like arrows pointing up and down, and an animation of them enlarging along with a number on the side. Originally, we are storing 128x128. When these stacked overlay square registers are scaling, I would like the numbers to go up as an animation to show that we are increasing it.

That's the end of Scene 11.