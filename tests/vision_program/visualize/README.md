# vision_program / visualize

A small standalone Python script to render the input/output 128*128 grids
embedded by every `tests/vision_program/<demo>/<demo>.c` test program in
its `run.log`.

## Usage

After running a demo via `run-test.sh`, point this script at the
resulting `run.log`:

```sh
python3 tests/vision_program/visualize/visualize.py \
    test_output/mudkip2d128small1bram1chain2lanescale/vision_program.sobel_edge-<TS>/run.log
```

By default this writes a PNG named `viz.png` next to the `run.log`.

Options:

* `--out PATH` - write the PNG to a specific path.
* `--show` - also display the figure interactively (requires a display).

## How it works

Each demo C program prints its 128*128 input/output grids using a
`dump_grid()` helper that emits the following plain-text format
between regular `printf` lines:

```
[GRID_DUMP_BEGIN] grid_in
<row 0: 128 space-separated decimal ints>
...
<row 127: 128 space-separated decimal ints>
[GRID_DUMP_END] grid_in
[GRID_DUMP_BEGIN] grid_out
...
[GRID_DUMP_END] grid_out
```

The visualiser scans `run.log` for those sentinel pairs, parses each
block into a `numpy` array, and renders all grids found in a single
horizontal-strip matplotlib figure with a `gray` colormap clamped to
the int8 range −128..127.

For `matvec_fc_relu`, the dump contains the weight matrix `A`, the
replicated `x` and `b` grids, and the per-row output column. For
`matmul_via_vt`, the dump contains `A`, `B`, the transposed `BT`, and
the full output `C`.

## Why not a flag on `run-test.sh`?

`run-test.sh` is shared with the repo's CI flow and intentionally
free of Python dependencies. The dump markers are inert plain text in
`run.log`, so adding them costs nothing for non-visualisation users.
The visualiser is opt-in, post-processing only - run it on any past
`run.log` you still have on disk.

## Dependencies

```
numpy
matplotlib
```

Both are typically already present on a developer host (or installable
via `pip install --user numpy matplotlib`).
