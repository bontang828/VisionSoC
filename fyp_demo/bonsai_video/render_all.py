"""Render every BONSAI scene in order and concatenate into one mp4.

Usage:
    python render_all.py            # low quality draft (480p15), fast
    python render_all.py -qm        # medium (720p30)
    python render_all.py -qh        # high (1080p60)

Run from the bonsai_video/ directory.
"""
import os
import subprocess
import sys

import imageio_ffmpeg

HERE = os.path.dirname(os.path.abspath(__file__))
FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()

# (file, SceneClass) in playback order
SCENES = [
    ("s1_title.py", "Title"),
    ("s1_5_kernel_demo.py", "KernelDemo"),
    ("s2_cat.py", "TheImage"),
    ("s3_5_compare.py", "ThreeWayCompare"),
    ("s3_6_contribution.py", "ContributionArch"),
    ("s3_7_fpga.py", "ContributionFPGA"),
    ("s3_8_eval.py", "ContributionEval"),
    ("s3_9_howitworks.py", "HowItWorks"),
    ("s3_10_soc_dataflow.py", "SocDataflow"),
    # s4/s5 retired: the top-level SoC animation (3.10) covers the capture,
    # and s6 absorbs the zoom-in + row-by-row register-file load.
    ("s6_programmability.py", "Programmability"),
    ("s7_row_col.py", "RowColProcessing"),
    ("s7_5_heterogeneous.py", "HeterogeneousProcessing"),
    ("s8_expand_registers.py", "ExpandRegisters"),
    ("s10_collapse.py", "CollapseRegisters"),
    ("s11_scalable.py", "Scalable"),
    ("s12_compute_scale.py", "ComputeScale"),
    ("s13_linear.py", "LinearScaling"),
    ("s22_summary.py", "Summary"),
    # Scenes below are retired: their content is covered earlier now
    # (s14/s19 recaps; s15-s18 decoder story lives in s6; s20/s21 die
    # channels live in s3_10).
    # ("s14_arch.py", "ArchOverview"),
    # ("s15_decoder_in.py", "DecoderIn"),
    # ("s16_decoder_multi.py", "DecoderMulti"),
    # ("s17_instr_list.py", "InstrList"),
    # ("s18_compile.py", "Compile"),
    # ("s19_arch.py", "ArchRecap"),
    # ("s20_die_channels.py", "DieChannels"),
    # ("s21_two_dies.py", "TwoDies"),
]

QUALITY_DIR = {"-ql": "480p15", "-qm": "720p30", "-qh": "1080p60", "-qk": "2160p60"}


def main():
    quality = "-ql"
    for a in sys.argv[1:]:
        if a in QUALITY_DIR:
            quality = a
    res_dir = QUALITY_DIR[quality]

    produced = []
    for fname, cls in SCENES:
        path = os.path.join("scenes", fname)
        print(f"\n=== Rendering {cls} ({fname}) ===")
        r = subprocess.run(
            [sys.executable, "-m", "manim", quality, "--disable_caching", path, cls],
            cwd=HERE,
        )
        if r.returncode != 0:
            print(f"!! FAILED: {cls}")
            sys.exit(1)
        mp4 = os.path.join(HERE, "media", "videos",
                           os.path.splitext(fname)[0], res_dir, f"{cls}.mp4")
        if not os.path.exists(mp4):
            print(f"!! Missing output: {mp4}")
            sys.exit(1)
        produced.append(mp4)

    # write concat list
    list_path = os.path.join(HERE, "concat_list.txt")
    with open(list_path, "w") as f:
        for mp4 in produced:
            f.write(f"file '{mp4}'\n")

    out = os.path.join(HERE, "bonsai_demo.mp4")
    print(f"\n=== Concatenating -> {out} ===")
    subprocess.run(
        [FFMPEG, "-y", "-f", "concat", "-safe", "0", "-i", list_path,
         "-c", "copy", out],
        check=True,
    )
    print(f"\nDONE: {out}")


if __name__ == "__main__":
    main()
