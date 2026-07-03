"""Scene 3.5 - Three-way comparison, introduced STAGE BY STAGE.
Layout: Top = Conventional, Bottom = On-Sensor, Middle = Near-Sensor (our focus).
Reveal order: Conventional -> On-Sensor -> Near-Sensor (revealed last).
One small cat (from Scene 2) on the left feeds each pipeline via a dragged copy.
Then the cat leaves and two gradient axes appear (power usage, programmability),
and a rectangle highlights the near-sensor (BONSAI) approach.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Square, Text, Arrow, Line, Polygon, RoundedRectangle,
    SurroundingRectangle, FadeIn, FadeOut, Create, GrowArrow,
    ReplacementTransform, interpolate_color,
    LEFT, RIGHT, UP, DOWN,
)
from common.layout import make_cat_photo, CAT_SMALL_POS, CAT_SMALL_W
from common.icons import camera_sensor, chip, lightning
from common.palette import BG, FG, MUTED, TEAL, AMBER, BLUE, GREEN, PURPLE, RED, SANS, MONO, caption
from scenes.s3_conventional import conventional_pipeline, conv_label, die_around, CONV_Y, PIPE_X0

NS_Y, OS_Y = 0.0, -2.6
LBL_X = -4.6


def _p(x, y):
    return np.array([x, y, 0.0])


def data_block(color=AMBER):
    return Text("(3,7) (1,4)\n(8,2) (5,9)", font=MONO, font_size=15, color=color)


def power_label(center, text="low power", color=GREEN, fs=13):
    b = lightning(0.22, color)
    t = Text(text, font=SANS, font_size=fs, color=color,
             weight="BOLD").next_to(b, RIGHT, buff=0.06)
    return VGroup(b, t).move_to(center)


def data_chip(color):
    """The travelling result block, on a solid backing for visibility."""
    txt = data_block(color)
    bg = RoundedRectangle(width=txt.width + 0.2, height=txt.height + 0.16,
                          corner_radius=0.06, stroke_width=0).set_fill(BG, 0.9)
    return VGroup(bg.move_to(txt), txt)


def done_tick(target, corner=UP + RIGHT):
    """A green 'finished' tick at a corner of a chip - trip complete."""
    off = corner * 0.24
    return Text("✓", font=SANS, font_size=30, color=GREEN, weight="BOLD").move_to(
        target.get_corner(corner) + off)


def crossed_gpu(center):
    """A ghost GPU with a red cross - this pipeline has none."""
    g = chip("GPU", w=1.0, h=0.85, color=MUTED)
    g.move_to(center).set_opacity(0.5)
    c1 = Line(g.get_corner(UP + LEFT), g.get_corner(DOWN + RIGHT),
              stroke_width=5).set_stroke(RED, opacity=0.9)
    c2 = Line(g.get_corner(UP + RIGHT), g.get_corner(DOWN + LEFT),
              stroke_width=5).set_stroke(RED, opacity=0.9)
    return VGroup(g, c1, c2)


def _multi_color(colors, t):
    t = min(max(t, 0.0), 1.0)
    if len(colors) == 1:
        return colors[0]
    s = t * (len(colors) - 1)
    i = min(int(s), len(colors) - 2)
    return interpolate_color(colors[i], colors[i + 1], s - i)


def grad_axis(x, y_tail, y_head, colors, sw=11, n=48, tip=0.36):
    """A vertical gradient arrow built from many small coloured segments (so the
    gradient renders clearly), with a filled triangular tip at the head."""
    d = 1 if y_head > y_tail else -1
    body_end = y_head - d * tip
    segs = VGroup()
    for i in range(n):
        ya = y_tail + (body_end - y_tail) * i / n
        yb = y_tail + (body_end - y_tail) * (i + 1) / n
        segs.add(Line(_p(x, ya), _p(x, yb), stroke_width=sw).set_color(_multi_color(colors, i / (n - 1))))
    tip_m = Polygon(_p(x, y_head), _p(x - tip * 0.5, body_end), _p(x + tip * 0.5, body_end),
                    stroke_width=0).set_fill(colors[-1], 1)
    return VGroup(segs, tip_m)


def proc_grid(center):
    cells = VGroup(*[Square(0.26, stroke_color=AMBER, stroke_width=1.5).set_fill(AMBER, 0.12)
                     for _ in range(9)])
    cells.arrange_in_grid(rows=3, cols=3, buff=0.07)
    box = RoundedRectangle(width=cells.width + 0.3, height=cells.height + 0.3, corner_radius=0.08,
                           stroke_color=AMBER, stroke_width=2).set_fill(AMBER, 0.05).move_to(cells)
    return VGroup(box, cells).move_to(center)


class ThreeWayCompare(Scene):
    def construct(self):
        cat = make_cat_photo(CAT_SMALL_W).move_to(CAT_SMALL_POS)   # carried from Scene 2
        self.add(cat)

        def drag(w):
            c = make_cat_photo(w).move_to(cat)
            self.add(c)
            return c

        # ============= TOP: conventional (revealed first) ================
        conv = conventional_pipeline()
        conv_lbl = conv_label()
        conv_s_lbl = Text("sensor", font=SANS, font_size=13, color=BLUE)
        conv_s_lbl.next_to(conv.sensor_die, UP, buff=0.08)
        capA = caption("Conventional: the image travels sensor -> CPU -> GPU, then back.")
        self.play(FadeIn(conv_lbl), FadeIn(capA), run_time=0.7)
        c1 = drag(0.5)
        self.play(FadeIn(conv.stage_sensor), FadeIn(conv_s_lbl),
                  c1.animate.move_to(conv.sensor), run_time=1.0)
        self.play(FadeIn(conv.stage_cpu), c1.animate.move_to(conv.cpu), run_time=1.0)
        self.play(FadeIn(conv.stage_gpu), c1.animate.move_to(conv.gpu), run_time=1.0)
        # the GPU turns the image into a compact result and returns it to the
        # CPU (crossfade: c1 is an ImageMobject and cannot Transform into text)
        conv_data = data_chip(AMBER).scale(0.9).move_to(conv.gpu)
        self.play(FadeOut(c1, scale=0.8), FadeIn(conv_data, scale=1.1),
                  run_time=0.7)
        ret_y = CONV_Y - 0.9
        conv_ret = Arrow(_p(conv.gpu.get_center()[0], ret_y),
                         _p(conv.cpu.get_center()[0], ret_y), buff=0.1,
                         stroke_width=3,
                         max_tip_length_to_length_ratio=0.12).set_color(AMBER)
        self.play(GrowArrow(conv_ret), conv_data.animate.move_to(conv.cpu),
                  run_time=1.0)
        # bottom-right corner: the top is occupied by the "high power" bolts
        t_conv = done_tick(conv.cpu, corner=DOWN + RIGHT)
        self.play(FadeIn(t_conv, scale=1.4), FadeOut(conv_data), run_time=0.7)

        # ============= BOTTOM: on-sensor (revealed second) ===============
        grid = proc_grid(_p(PIPE_X0, OS_Y))
        grid_d = die_around(grid, AMBER, pad_w=0.18, pad_h=0.18)
        grid_lbl = Text("processor per pixel", font=SANS, font_size=18, color=AMBER).next_to(grid, UP, buff=0.34)
        o_cpu = chip("CPU", w=1.0, h=0.85, color=MUTED).move_to(_p(PIPE_X0 + 3.2, OS_Y))
        oc_d = die_around(o_cpu, MUTED)
        o_arrow = Arrow(grid.get_right(), o_cpu.get_left(), buff=0.15, stroke_width=3, max_tip_length_to_length_ratio=0.15).set_color_by_gradient(GREEN, TEAL)
        o_lp = power_label(o_arrow.get_center() + UP * 0.42, "low power")
        o_bw = Text("low bandwidth", font=SANS, font_size=12, color=GREEN,
                    weight="BOLD").next_to(o_arrow, DOWN, buff=0.08)
        o_data = data_chip(GREEN).move_to(grid)
        # a translucent camera lens over the grid: the sensor is IN the array
        cam_ov = camera_sensor(0.85).move_to(grid)
        cam_ov.set_opacity(0.3)
        os_lbl = Text("On-sensor\nprocessing", font=SANS, font_size=19, color=MUTED,
                      weight="BOLD", line_spacing=0.7).move_to(_p(LBL_X, OS_Y))
        os = VGroup(grid, grid_d, grid_lbl, cam_ov, o_cpu, oc_d, o_arrow,
                    o_lp, o_bw, o_data)

        capB = caption("On-sensor: a processor in every pixel, results go straight to the CPU.")
        self.play(FadeIn(os_lbl), FadeOut(capA), FadeIn(capB), run_time=0.7)
        c3 = drag(0.55)
        self.play(FadeIn(grid, grid_d, grid_lbl), c3.animate.move_to(grid), run_time=1.0)
        self.play(FadeIn(cam_ov), run_time=0.6)
        self.play(FadeOut(c3), FadeIn(o_data), run_time=0.6)
        self.play(FadeIn(o_arrow, o_cpu, oc_d, o_lp, o_bw),
                  o_data.animate.move_to(o_cpu), run_time=1.0)
        t_os = done_tick(o_cpu)
        gpu_x_os = crossed_gpu(_p(PIPE_X0 + 5.9, OS_Y))
        self.play(FadeIn(t_os, scale=1.4), FadeOut(o_data), FadeIn(gpu_x_os[0]),
                  run_time=0.7)
        os.remove(o_data)   # detach: FadeOut restores opacity on removal, and
                            # the final group fade would flash it back otherwise
        self.play(Create(gpu_x_os[1]), Create(gpu_x_os[2]), run_time=0.6)

        # ============= MIDDLE: near-sensor (revealed LAST - our focus) ====
        n_sensor = camera_sensor(0.65).move_to(_p(PIPE_X0, NS_Y))
        n_s_lbl = Text("sensor", font=SANS, font_size=13, color=BLUE)
        n_s_lbl.next_to(n_sensor, UP, buff=0.1)
        n_scratch = chip("Scratchpad\nMemory", w=1.45, h=0.95, color=GREEN, font_size=15).move_to(_p(PIPE_X0 + 2.1, NS_Y))
        n_bons = chip("BONSAI\nProcessor", w=1.5, h=1.0, color=PURPLE, font_size=15).move_to(_p(PIPE_X0 + 4.6, NS_Y))
        n_inner = VGroup(n_sensor, n_scratch, n_bons)
        n_die = die_around(n_inner, TEAL, pad_w=0.4, pad_h=0.4)
        n_die_r = RoundedRectangle(width=n_inner.width + 0.8, height=n_inner.height + 0.8).move_to(n_inner)
        n_a1 = Arrow(n_sensor.get_right(), n_scratch.get_left(), buff=0.05, stroke_width=2.5, max_tip_length_to_length_ratio=0.3).set_color_by_gradient(GREEN, TEAL)
        n_a2 = Arrow(n_scratch.get_right(), n_bons.get_left(), buff=0.05, stroke_width=2.5, max_tip_length_to_length_ratio=0.3).set_color_by_gradient(GREEN, TEAL)
        n_cpu = chip("CPU", w=1.0, h=0.85, color=MUTED).move_to(_p(PIPE_X0 + 7.6, NS_Y))
        nc_d = die_around(n_cpu, MUTED)
        n_out = Arrow(n_die_r.get_right(), n_cpu.get_left(), buff=0.1, stroke_width=3, max_tip_length_to_length_ratio=0.12).set_color_by_gradient(BLUE, TEAL)
        ua1 = power_label(_p(n_a1.get_center()[0], NS_Y + 0.66), "ultra low power", fs=11)
        ua2 = power_label(_p(n_a2.get_center()[0], NS_Y + 0.66), "ultra low power", fs=11)
        n_lp = power_label(n_out.get_center() + UP * 0.42, "low power")
        n_bw = Text("low bandwidth", font=SANS, font_size=12, color=BLUE,
                    weight="BOLD").next_to(n_out, DOWN, buff=0.08)
        n_data = data_chip(BLUE).scale(0.9).move_to(n_bons)   # inside the Bonsai box
        ns_lbl = Text("Near-sensor\nprocessing", font=SANS, font_size=19, color=TEAL,
                      weight="BOLD", line_spacing=0.7).move_to(_p(LBL_X, NS_Y))
        ns = VGroup(n_sensor, n_s_lbl, n_scratch, n_bons, n_die, n_a1, n_a2,
                    ua1, ua2, n_cpu, nc_d, n_out, n_lp, n_bw, n_data)

        capC = caption("Near-sensor: a full processor beside the sensor, on the same die.")
        self.play(FadeIn(ns_lbl), FadeOut(capB), FadeIn(capC), run_time=0.7)
        c2 = drag(0.4)
        self.play(FadeIn(n_sensor, n_s_lbl, n_die), c2.animate.move_to(n_sensor), run_time=1.0)
        self.play(FadeIn(n_a1, n_scratch, ua1), c2.animate.move_to(n_scratch), run_time=1.0)
        self.play(FadeIn(n_a2, n_bons, ua2), c2.animate.move_to(n_bons), run_time=1.0)
        self.play(FadeOut(c2), FadeIn(n_data), run_time=0.6)
        self.play(FadeIn(n_out, n_cpu, nc_d, n_lp, n_bw),
                  n_data.animate.move_to(n_cpu), run_time=1.0)
        t_ns = done_tick(n_cpu)
        gpu_x_ns = crossed_gpu(_p(PIPE_X0 + 7.6, -1.58))
        self.play(FadeIn(t_ns, scale=1.4), FadeOut(n_data), FadeIn(gpu_x_ns[0]),
                  run_time=0.7)
        ns.remove(n_data)   # same detach as the on-sensor data block
        self.play(Create(gpu_x_ns[1]), Create(gpu_x_ns[2]), run_time=0.6)

        cap = caption("Three ways to process a frame, trading off power and programmability.")
        self.play(FadeOut(capC), FadeIn(cap), run_time=0.7)
        self.wait(2.0)

        # ============= comparison axes (no shift) ========================
        # the crossed-out GPUs leave before the axes claim the side space
        self.play(FadeOut(cap), FadeOut(cat),
                  FadeOut(gpu_x_os), FadeOut(gpu_x_ns), run_time=0.7)

        grad = [RED, AMBER, GREEN]
        power = grad_axis(-6.6, 3.0, -3.0, grad)      # top (High/Bad) -> bottom (Low/Good)
        p_ttl = Text("Power Usage", font=SANS, font_size=22, color=FG,
                     weight="BOLD").rotate(np.pi / 2).move_to(_p(-6.15, 0.0))
        p_top = Text("High (Bad)", font=SANS, font_size=20, color=RED).next_to(_p(-6.6, 3.0), RIGHT, buff=0.12)
        p_bot = Text("Low (Good)", font=SANS, font_size=20, color=GREEN).next_to(_p(-6.6, -3.0), RIGHT, buff=0.12)

        prog = grad_axis(6.6, -3.0, 3.0, grad)         # bottom (Low/Bad) -> top (High/Good)
        pr_ttl = Text("Programmability", font=SANS, font_size=22, color=FG,
                      weight="BOLD").rotate(-np.pi / 2).move_to(_p(6.15, 0.0))
        pr_top = Text("High (Good)", font=SANS, font_size=20, color=GREEN).next_to(_p(6.6, 3.0), LEFT, buff=0.12)
        pr_bot = Text("Low (Bad)", font=SANS, font_size=20, color=RED).next_to(_p(6.6, -3.0), LEFT, buff=0.12)

        self.play(Create(power), FadeIn(VGroup(p_ttl, p_top, p_bot)), run_time=0.9)
        self.play(Create(prog), FadeIn(VGroup(pr_ttl, pr_top, pr_bot)), run_time=0.9)

        # highlight the near-sensor (BONSAI) approach
        hl = SurroundingRectangle(VGroup(ns_lbl, ns), color=TEAL, buff=0.22, corner_radius=0.12)
        hl.set_stroke(TEAL, 3).set_fill(TEAL, 0.05)
        hl_lbl = Text("BONSAI approach", font=SANS, font_size=26, color=TEAL,
                      weight="BOLD").next_to(hl, DOWN, buff=0.12)
        cap_ax = caption("BONSAI hits the sweet spot: low power and high programmability.")
        self.play(Create(hl), FadeIn(hl_lbl), FadeIn(cap_ax), run_time=0.8)
        self.wait(2.4)
        self.play(FadeOut(VGroup(conv, conv_lbl, conv_s_lbl, ns, ns_lbl, os, os_lbl,
                                 hl, hl_lbl, cap_ax, conv_ret, t_conv, t_os, t_ns,
                                 power, p_ttl, p_top, p_bot, prog, pr_ttl, pr_top, pr_bot)), run_time=0.9)
