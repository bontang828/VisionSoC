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


**scene 3.5** I would like to create a new scene between Scene 3 and Scene 4 that combines elements from both while adding several extra features. The goal is to compare our near-sensor architecture against the conventional pipeline and the on-sensor approach.

Here is the layout and sequence for this complex scene:

The Conventional Pipeline (Top)
(a) Start with a cat image in the center of the screen, then shift it to the left.
(b) Display a camera sensor on the right side with two lines running toward it from the image.
© Show high power usage indicators, noting that data must be sent to both the CPU and GPU.
(d) Move this entire assembly to the top of the screen and label it "Conventional Pipeline" on the left-hand side.

The On-Sensor Approach (Bottom)
(a) Below the conventional pipeline, display a "processor-per-pixel" setup (represented by a 3x3 grid of processors).
(b) Run two lines from the cat image to this grid.
© Draw an arrow pointing from the grid to the CPU labeled "Result Only" to show that only processed results leave the sensor.
(d) Add a new power symbol and move this entire pipeline to the bottom of the display.

The Near-Sensor Architecture (Middle)
(a) Position this pipeline in the center, between the other two.
(b) Use the "near-sensor die" (incorporating the camera sensor).
© Important: Label the components as "Memory" (instead of buffer) and "Bonsai Architecture."
(d) Run three lines from the cat image to this die to represent the pipeline.
(e) Draw an arrow pointing to the CPU to show the results output.
(f) Label this pipeline "Near-Sensor" on the left-hand side.

Final Comparison Animation
(a) Once all three pipelines are visible, the cat image should move off-screen (since it is already imported into the sensors).
(b) Shift the three pipelines together into the center of the monitor.
© Add two vertical comparison arrows on either side of the group:
i. Left Side (Power Consumption): A downward-pointing arrow (top to bottom). Label the top as "High (Bad)" in red and the bottom as "Low (Good)" in green, using a temperature-style color gradient.
ii. Right Side (Programmability): An upward-pointing arrow (bottom to top). Label the bottom as "Low (Bad)" in red and the top as "High (Good)" in green.
(d) Add animations for these two arrows to conclude the scene.


## Contributions section (scenes 3.6 – 3.8)

A three-scene "Our Contributions" interlude that sits **right after the 3-pipeline
comparison** (the power-usage / programmability arrows scene) and before Scene 4
(the near-sensor die deep-dive). Each scene is one contribution, with a persistent
kicker "CONTRIBUTION 1 / 3", "2 / 3", "3 / 3" so the audience tracks progress.

**Scene 3.6 — Contribution 1: the architecture**
Headline: *A generalised 2D spatial near-sensor chip architecture — **BONSAI***.
Three feature cards reveal one by one, each with a small icon:
- **Heterogeneous processing** — one fabric runs many different kernels.
- **Easy programmability** — standard RISC-V vector (RVV) ISA.
- **Row & column image-plane processing** — operate along rows *and* columns natively.

**Scene 3.7 — Contribution 2: live camera FPGA prototype**
Headline: *Live camera FPGA prototype*. Show the **AMD Kria KV260** board photo
(`kv260_image_background_removed.png`) on the right; bullets reveal on the left:
- AMD Kria KV260 (real hardware).
- Real-time **30 FPS** over HDMI.
- **Time-multiplexed** execution (logical rows mapped onto physical lanes).
- **Demonstration kernels** running live from the camera.

**Scene 3.8 — Contribution 3: evaluation**
Headline: *Evaluation of a vector-instruction-based 2D image-plane processor*.
Left: a schematic **linear resource-scaling** plot (ties back to Scene 13).
Right: a checklist of what was evaluated:
- Row & column kernels.
- Whole image processed in one instruction.
- Real-time on FPGA.

**Scene 4:** I would like to show a similar concept to Scene 3, starting with the same cat image.
4A. On the right-hand side, show our new near-sensor die. This die, displayed on the right-hand side screen, includes a camera sensor, a buffer, and our Bonsai processor, along with the interconnects we previously discussed in another document.
4B. Show the cat again with two lines drawn towards the camera sensor, indicating that this sensor is capturing the cat.

**Scene 5:** We zoom into the Bonsai.
5A. Show that the cat image is being captured by the sensor, loaded into the buffer, and then moved towards the Bonsai architecture.
5B. We shift the Bonsai architecture to the center of the frame and zoom in to see the clear architecture of the floor plan. It shows square register planes stacked together, overlaying each other. Below them, we have the compute lanes. The compute lane is connected to a load-store unit, which has arrows drawn from the outer to the inner parts of the architecture to show communication between the outer buffer and the on-chip architecture.

**Scene 8:** We zoom into those square register files. We break them down because they were overlayed. Now, we have an animation of them filling up the entire screen. We break them down into 32 2D square registers, as we have 32 stacked together. We won't show all 32 stacked, but multiple ones to convey that there are many. When we expand them in Scene 8 for the animation, we see 32 of them arranged on the screen.

**Scene 9:** We see the naming of each of these square registers from V0 up to V31 under each square register plane.

**Scene 10:** We collapse all of the square register planes back to the stacked overlay version, just as we showed the architecture previously, because we are about to shift into another subsystem in the architecture.

**Scene 11:** Before we zoom into the compute subsystem, with the stacked overlay square registers, we show that they are scalable in the horizontal and vertical directions. For scalability, I would like arrows pointing up and down, and an animation of them enlarging along with a number on the side. Originally, we are storing 128x128. When these stacked overlay square registers are scaling, I would like the numbers to go up as an animation to show that we are increasing it.

That's the end of Scene 11.
Now let's continue on the markdown file.

Scene 12:
We are enlarging the compute lane and showing it is scalable by duplicating the compute lane in a vertically downwards direction. Instead of overlapping, they should be duplicating downwards. We can add "..." to represent that there are many more to come, or perhaps just lower the opacity.

Scene 13:
Show that it is scalable and the resource use is linear.

Scene 14:
Zoom back out from the compute lane and show the architecture again. This should include:
1. The stacked register file
2. The compute lane
3. The LSU
4. The decoder

Scene 15:
Zoom into the decoder. Show one arrow pointing from left to right into the decoder to represent instructions coming in.

Scene 16:
Zoom further into the decoder to show that it can process multiple instructions. It supports RISC-V RVV ISA instructions.

Scene 17:
The instruction blob expands into a larger list showing examples:
1. vf.add v1, v2, v3 (using the different register numbers)
2. vsub with their respective registers
3. vshift
4. vgather, etc.

Scene 18:
In this list, each row of instructions has an arrow pointing from left to right to show they compile into machine code. Add text stating it is compatible with open-source compilers, such as LLVM and GNU.

Scene 19:
Zoom back out from the decoder and show the architecture again, displaying the stacked register file, the compute lane, the LSU, and the decoder.

Scene 20:
Zoom out even further to show the near-sensor die again. We should see:
1. The camera sensor
2. The buffer
3. The BONSAI architecture

Two arrows appear showing the data channel and the instruction channel going out of the near-sensor die. I would like the data channel to connect to the buffer (since the buffer is essentially a large scratchpad memory). The instruction channel should be coming out of or going into the BONSAI architecture.

Scene 21:
Zooming out a bit more, we can see that these two channels connect to a scalar core SOC die on the right-hand side. We should see the two dies side-by-side, connected by these two channels.




