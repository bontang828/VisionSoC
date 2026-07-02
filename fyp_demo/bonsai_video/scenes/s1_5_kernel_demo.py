"""Scene 1.5 - Live kernel demo, right after the title page (matched cut).
The title card (tree + BONSAI + subtitle) gets a frame, shrinks to the left,
and the real FPGA prototype footage (optical flow kernel) plays beside it for
its full length. Sensor data flows from the footage into the card, processed
data flows back, and the bonsai leaves pulse like a loading indicator while
the demo runs."""
import glob
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from PIL import Image as PILImage
from manim import (
    Scene, Group, VGroup, Text, RoundedRectangle, Arrow, ImageMobject,
    FadeIn, FadeOut, Create, GrowArrow, LaggedStart, there_and_back,
    UP, DOWN, LEFT, RIGHT,
)
from manim.utils.images import change_to_rgba_array
from common.icons import bonsai_tree
from common.palette import FG, TEAL, AMBER, BLUE, SANS, caption

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOV = os.path.join(HERE, "kernel_demo.mov")
FRAMES_DIR = os.path.join(HERE, "common", "assets", "kernel_demo_frames")
VIDEO_FPS = 30       # extraction rate; playback clock is real time
VIDEO_H = 5.6        # displayed height of the portrait footage


def _p(x, y):
    return np.array([x, y, 0.0])


def _ensure_frames():
    """The footage plays as a JPEG frame sequence (no cv2 dependency).
    Extract it from kernel_demo.mov on first use."""
    files = sorted(glob.glob(os.path.join(FRAMES_DIR, "kd_*.jpg")))
    if files:
        return files
    import imageio_ffmpeg
    os.makedirs(FRAMES_DIR, exist_ok=True)
    subprocess.run(
        [imageio_ffmpeg.get_ffmpeg_exe(), "-y", "-i", MOV,
         "-vf", f"fps={VIDEO_FPS},scale=-2:960", "-q:v", "3", "-an",
         os.path.join(FRAMES_DIR, "kd_%04d.jpg")],
        check=True,
    )
    return sorted(glob.glob(os.path.join(FRAMES_DIR, "kd_*.jpg")))


class KernelDemo(Scene):
    def construct(self):
        files = _ensure_frames()
        video_len = len(files) / VIDEO_FPS

        # --- carry-in: EXACT end state of the title scene ---
        tree = bonsai_tree(scale=1.3).move_to(UP * 1.1)
        title = Text("BONSAI", font=SANS, font_size=72, color=FG, weight="BOLD")
        title.next_to(tree, DOWN, buff=0.5)
        sub = Text("A 2D RISC-V Vision Processor", font=SANS, font_size=30, color=TEAL)
        sub.next_to(title, DOWN, buff=0.25)
        self.add(tree, title, sub)
        self.wait(0.3)

        # --- 1) "Live Demo" heading + a frame fades in around the card ---
        heading = Text("Live Demo", font=SANS, font_size=38, color=FG,
                       weight="BOLD").to_corner(UP + LEFT, buff=0.55)
        inner = VGroup(tree, title, sub)
        frame = RoundedRectangle(
            width=inner.width + 0.8, height=inner.height + 0.7,
            corner_radius=0.18, stroke_color=TEAL, stroke_width=3,
        ).move_to(inner)
        self.play(FadeIn(heading, shift=DOWN * 0.15), run_time=0.6)
        self.play(Create(frame), run_time=0.9)
        self.wait(0.3)

        # --- 2) card shrinks to the left; the live footage joins on the right
        card = VGroup(frame, tree, title, sub)
        self.play(card.animate.scale(0.72).move_to(_p(-3.9, -0.1)), run_time=1.2)

        video = ImageMobject(files[0]).scale_to_fit_height(VIDEO_H)
        video.move_to(_p(3.3, -0.15))
        vframe = RoundedRectangle(
            width=video.width + 0.12, height=video.height + 0.12,
            corner_radius=0.1, stroke_color=FG, stroke_width=2,
        ).move_to(video)
        cap = caption("Optical flow kernel running live on the BONSAI FPGA prototype")
        self.play(FadeIn(video), Create(vframe), FadeIn(cap), run_time=1.0)

        # play the footage in real time from here: swap the pixel array by clock
        clock = {"t": 0.0, "idx": 0}

        def _advance(m, dt):
            clock["t"] += dt
            i = min(int(clock["t"] * VIDEO_FPS), len(files) - 1)
            if i != clock["idx"]:
                clock["idx"] = i
                arr = np.asarray(PILImage.open(files[i]).convert("RGB"))
                m.pixel_array = change_to_rgba_array(arr, m.pixel_array_dtype)

        video.add_updater(_advance)
        used = 0.0

        # --- 3) sensor data: footage -> BONSAI card ---
        self.wait(0.4)
        vx = video.get_left()[0] - 0.08
        cx = card.get_right()[0] + 0.1
        a_in = Arrow(_p(vx, -1.75), _p(cx, -1.4), color=BLUE, buff=0,
                     stroke_width=5, max_tip_length_to_length_ratio=0.12)
        l_in = Text("Sensor data", font=SANS, font_size=22,
                    color=BLUE).next_to(a_in, DOWN, buff=0.18)
        self.play(GrowArrow(a_in), FadeIn(l_in), run_time=1.0)
        self.wait(0.5)

        # --- 4) processed data: BONSAI card -> footage ---
        a_out = Arrow(_p(cx, 1.0), _p(vx, 1.35), color=AMBER, buff=0,
                      stroke_width=5, max_tip_length_to_length_ratio=0.12)
        l_out = Text("Processed data", font=SANS, font_size=22,
                     color=AMBER).next_to(a_out, UP, buff=0.18)
        self.play(GrowArrow(a_out), FadeIn(l_out), run_time=1.0)
        self.wait(0.4)
        used += 0.4 + 1.0 + 0.5 + 1.0 + 0.4

        # --- 5) bonsai "loading" pulse while the demo runs its full length ---
        leaves = VGroup(*tree.submobjects[3:6])
        CYCLE = 0.85    # fast pulse: BONSAI is visibly busy processing
        n_cycles = max(1, int((video_len - used) // CYCLE))
        for _ in range(n_cycles):
            self.play(LaggedStart(
                *[leaf.animate(rate_func=there_and_back)
                  .set_fill(opacity=0.15).scale(0.88) for leaf in leaves],
                lag_ratio=0.3), run_time=CYCLE)
        used += n_cycles * CYCLE
        self.wait(max(0.3, video_len - used + 0.3))

        video.clear_updaters()
        self.play(FadeOut(Group(heading, card, video, vframe, cap,
                                a_in, l_in, a_out, l_out)), run_time=0.9)
