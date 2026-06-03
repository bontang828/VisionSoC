#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
output_dir="${script_dir}/../selected_output"
tex_file="${output_dir}/drawio_figures.tex"

usage() {
  cat <<'EOF'
Usage:
  ./export_drawio_pdf.sh                 Export every .drawio file in this directory
  ./export_drawio_pdf.sh NAME [...]      Export selected file(s)
  ./export_drawio_pdf.sh --tex-only      Regenerate TeX from ../selected_output/*.pdf

Examples:
  ./export_drawio_pdf.sh
  ./export_drawio_pdf.sh T1_abstract_arch
  ./export_drawio_pdf.sh T1_abstract_arch.drawio sobel_perf.drawio
  ./export_drawio_pdf.sh --tex-only

Output:
  PDFs are written to ../selected_output/
  LaTeX figure commands are written to ../selected_output/drawio_figures.tex

Environment:
  DRAWIO_BIN=/path/to/drawio             Override the draw.io CLI command
  DRAWIO_PDF_SCALE=1.15                  Scale the whole exported PDF
  DRAWIO_PDF_FONT_SCALE=1.10             Scale only fontSize values in a temp copy (default: 1.10)
  DRAWIO_TEXT_AS_SVG=1                   Convert HTML labels to SVG text in a temp copy
  DRAWIO_TEX_PATH=project_diagram/selected_output
                                       Path used inside \includegraphics
  DRAWIO_TEX_WIDTH=\textwidth            Width used inside \includegraphics
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

find_drawio_bin() {
  if [[ -n "${DRAWIO_BIN:-}" ]]; then
    command -v "$DRAWIO_BIN" >/dev/null 2>&1 || die "DRAWIO_BIN is set, but '$DRAWIO_BIN' is not executable"
    printf '%s\n' "$DRAWIO_BIN"
    return
  fi

  local candidate
  for candidate in drawio draw.io diagrams.net; do
    if command -v "$candidate" >/dev/null 2>&1; then
      local candidate_path
      candidate_path="$(command -v "$candidate")"

      if [[ "$candidate_path" == "/snap/bin/drawio" && -x "/snap/drawio/current/drawio" ]]; then
        printf '%s\n' "/snap/drawio/current/drawio"
      else
        printf '%s\n' "$candidate_path"
      fi
      return
    fi
  done

  die "draw.io CLI not found. Install drawio/diagrams.net desktop, or set DRAWIO_BIN=/path/to/drawio"
}

normalise_input() {
  local arg="$1"

  if [[ "$arg" == */* ]]; then
    die "pass only filenames from $(basename "$script_dir"), not paths: $arg"
  fi

  if [[ "$arg" != *.drawio ]]; then
    arg="${arg}.drawio"
  fi

  printf '%s\n' "${script_dir}/${arg}"
}

is_number() {
  [[ "$1" =~ ^[0-9]+([.][0-9]+)?$ ]]
}

prepare_export_source() {
  local source="$1"
  prepared_source="$source"

  if [[ "$font_scale" != "1" || "$text_as_svg" == "1" ]]; then
    [[ -n "$tmp_dir" ]] || tmp_dir="$(mktemp -d /tmp/drawio-pdf-export.XXXXXX)"

    prepared_source="${tmp_dir}/$(basename "$source")"
    cp "$source" "$prepared_source"

    if [[ "$font_scale" != "1" ]]; then
      FONT_SCALE="$font_scale" perl -0pi -e '
        my $scale = $ENV{FONT_SCALE};
        s/fontSize=([0-9]+(?:\.[0-9]+)?)/"fontSize=" . sprintf("%.2f", $1 * $scale)/ge;
        s/\.00(?=;|\")//g;
      ' "$prepared_source"
    fi

    if [[ "$text_as_svg" == "1" ]]; then
      perl -0pi -e '
        s/style="([^"]*)"/
          my $s = $1;
          $s =~ \/convertToSvg=\/ ? "style=\"$s\"" : "style=\"convertToSvg=1;$s\""
        /ge;
      ' "$prepared_source"
    fi
  fi
}

command_from_stem() {
  local stem="$1"

  case "$stem" in
    fabric_instruction_basics) printf '%s\n' figFabricInstructionBasics ;;
    fpga_system_top_t1style_v4) printf '%s\n' figFpgaSystemTop ;;
    image_to_vector_fabric_v3) printf '%s\n' figImageToVectorFabric ;;
    matmul_8bitraw_short_perf) printf '%s\n' figMatmulPerf ;;
    matmul_8bitraw_short_steps_v3) printf '%s\n' figMatmulSteps ;;
    optical_flow_perf) printf '%s\n' figOpticalFlowPerf ;;
    rvv_vs_t1_v2) printf '%s\n' figRvvVsTOne ;;
    sobel_kernel_steps_v2) printf '%s\n' figSobelSteps ;;
    sobel_perf) printf '%s\n' figSobelPerf ;;
    T1_abstract_arch_fpga) printf '%s\n' figTOneAbstractArchFpga ;;
    T1_abstract_arch_rtl) printf '%s\n' figTOneAbstractArchRtl ;;
    t1_vs_scamp5_v3) printf '%s\n' figTOneVsScamp ;;
    vrf_diagonal_banking_v3) printf '%s\n' figVrfDiagonalBanking ;;
    vrf_diagonal_banking_v5_per_word) printf '%s\n' figVrfDiagonalBankingVfivePerWord ;;
    *)
      STEM="$stem" perl -e '
        my %digit = (
          0 => "Zero", 1 => "One", 2 => "Two", 3 => "Three", 4 => "Four",
          5 => "Five", 6 => "Six", 7 => "Seven", 8 => "Eight", 9 => "Nine"
        );
        my $s = $ENV{STEM};
        $s =~ s/_v\d+$//;
        $s =~ s/[^A-Za-z0-9]+/ /g;
        my @parts = split /\s+/, $s;
        print "fig";
        for my $part (@parts) {
          next if $part eq "";
          $part =~ s/([0-9])/$digit{$1}/ge;
          $part = lc $part;
          $part = ucfirst $part;
          $part =~ s/[^A-Za-z]//g;
          print $part;
        }
        print "\n";
      '
      ;;
  esac
}

caption_from_stem() {
  local stem="$1"

  case "$stem" in
    fabric_instruction_basics) printf '%s\n' 'Instruction basics for the 2D T1 fabric. Highlighted the difference between horizontal and vertical mode execution with same RVV opcode' ;;
    fpga_system_top_t1style_v4) printf '%s\n' 'Top level FPGA system architecture for AMD Kria KV260. Highlighted with 3 major component group: Camera module, Processor System, Programmable Logic' ;;
    image_to_vector_fabric_v3) printf '%s\n' 'Demonstrate an image map to a 1D vector processor and a 2D vector processor' ;;
    matmul_8bitraw_short_perf) printf '%s\n' 'One frame of high level 8-bits MatMul kernel FPGA execution pipeline breakdown in time stages(top bar). Detailed breakdown of 8-bits MatMul kernel with individual RVV instruction performance in time stages(bottom bar). The 128 iteration of MatMul instructions(6-12) in the bottom bar are grouped together for clarity of total execution time used per instruction group. The actual MatMul instructions would looks like a fine breakdown of 128 iterations each use a fraction of the total T1 kernel time.' ;;
    matmul_8bitraw_short_steps_v3) printf '%s\n' 'Execution steps for matrix multiplication on the 2D vector architecture using RVV ISA.' ;;
    optical_flow_perf) printf '%s\n' 'One frame of high level Optical-flow kernel FPGA execution pipeline breakdown in time stages(top bar). Detailed breakdown of Optical-flow kernel with individual RVV instruction performance in time stages(bottom bar).' ;;
    rvv_vs_t1_v2) printf '%s\n' '[Diagram work in progress] Comparison between 1D RVV execution and the 2D RVV T1 fabric in data transaction footprints. Highlighted the reduction in data round trips required between transpose on 2D RVV compared to 1D RVV.' ;;
    sobel_kernel_steps_v2) printf '%s\n' 'Sobel kernel execution steps on the RVV 2D vector architecture.' ;;
    sobel_perf) printf '%s\n' 'One frame of high level Sobel kernel FPGA execution pipeline breakdown in time stages(top bar). Detailed breakdown of Sobel kernel with individual RVV instruction performance in time stages(bottom bar).' ;;
    T1_abstract_arch_fpga) printf '%s\n' 'Abstracted top level FPGA adaptation of the 2D T1 architecture. Highlighted with time multiplex lane processing, PS interface wrapper and scratch-pad memory subsystem.' ;;
    T1_abstract_arch_rtl) printf '%s\n' 'Abstracted top level RTL of 2D T1 architecture.' ;;
    t1_vs_scamp5_v3) printf '%s\n' 'Comparison between the T1 fabric and SCAMP-5 in programmability. Highlighted the data control abstractions with RISC-V RVV instructions and execution in a 2D plane.' ;;
    vrf_diagonal_banking_v3) printf '%s\n' 'Diagonal banking scheme used for the vector register file in 2D T1 memory subsystem.' ;;
    vrf_diagonal_banking_v5_per_word) printf '%s\n' 'Diagonal banking scheme used for the vector register file in 2D T1 memory subsystem.' ;;
    *)
      printf '%s\n' "$stem"
      ;;
  esac
}

write_tex_file() {
  local tex_path="${DRAWIO_TEX_PATH:-project_diagram/selected_output}"
  local tex_width="${DRAWIO_TEX_WIDTH:-\\textwidth}"
  local pdf stem command caption graphic_path
  local -a pdfs=()

  while IFS= read -r -d '' pdf; do
    pdfs+=("$pdf")
  done < <(find "$output_dir" -maxdepth 1 -type f -name '*.pdf' -print0 | sort -z)

  {
    printf '%% This file defines reusable figure commands.\n'
    printf '%% Include this once in the main file before chapter inputs:\n'
    printf '%% \\input{%s/drawio_figures}\n' "$tex_path"
    printf '%% Then call figures inside chapters using commands such as:\n'
    printf '%% \\figImageToVectorFabric\n'
    printf '%% Required package in main preamble:\n'
    printf '%% \\usepackage{graphicx}\n'
    printf '%% \\usepackage{float}\n'
    printf '%% Auto-generated by %s. Do not edit by hand.\n\n' "$(basename "$0")"

    for pdf in "${pdfs[@]}"; do
      stem="$(basename "$pdf" .pdf)"
      command="$(command_from_stem "$stem")"
      caption="$(caption_from_stem "$stem")"

      if [[ -n "$tex_path" ]]; then
        graphic_path="${tex_path}/${stem}"
      else
        graphic_path="$stem"
      fi

      cat <<EOF
\\newcommand{\\${command}}{%
\\begin{figure}[H]
    \\centering
    \\includegraphics[
        width=${tex_width}
    ]{${graphic_path}}
    \\caption{${caption}}
    \\label{fig:${stem}}
\\end{figure}
}

EOF
    done
  } > "$tex_file"
}

tex_only=0
declare -a requested=()

for arg in "$@"; do
  case "$arg" in
    -h|--help)
      usage
      exit 0
      ;;
    --tex-only)
      tex_only=1
      ;;
    *)
      requested+=("$arg")
      ;;
  esac
done

pdf_scale="${DRAWIO_PDF_SCALE:-}"
font_scale="${DRAWIO_PDF_FONT_SCALE:-1.10}"
text_as_svg="${DRAWIO_TEXT_AS_SVG:-0}"
tmp_dir=""
trap '[[ -z "$tmp_dir" ]] || rm -rf "$tmp_dir"' EXIT

if [[ -n "$pdf_scale" ]] && ! is_number "$pdf_scale"; then
  die "DRAWIO_PDF_SCALE must be a positive number, got: $pdf_scale"
fi

if ! is_number "$font_scale"; then
  die "DRAWIO_PDF_FONT_SCALE must be a positive number, got: $font_scale"
fi

if [[ "$text_as_svg" != "0" && "$text_as_svg" != "1" ]]; then
  die "DRAWIO_TEXT_AS_SVG must be 0 or 1, got: $text_as_svg"
fi

declare -a sources=()

if [[ "$tex_only" -eq 1 ]]; then
  [[ "${#requested[@]}" -eq 0 ]] || die "--tex-only rebuilds from all PDFs in $output_dir and does not accept filenames"
  mkdir -p "$output_dir"
  write_tex_file
  printf 'Done. LaTeX figure commands are in %s\n' "$tex_file"
  exit 0
fi

if [[ "${#requested[@]}" -eq 0 ]]; then
  while IFS= read -r -d '' file; do
    sources+=("$file")
  done < <(find "$script_dir" -maxdepth 1 -type f -name '*.drawio' -print0 | sort -z)
else
  for arg in "${requested[@]}"; do
    sources+=("$(normalise_input "$arg")")
  done
fi

[[ "${#sources[@]}" -gt 0 ]] || die "no .drawio files found in $script_dir"

for source in "${sources[@]}"; do
  [[ -f "$source" ]] || die "input file does not exist: $(basename "$source")"
done

drawio_bin="$(find_drawio_bin)"
drawio_cmd=("$drawio_bin")

if [[ -z "${DISPLAY:-}" ]]; then
  if command -v xvfb-run >/dev/null 2>&1; then
    drawio_cmd=(xvfb-run -a "$drawio_bin")
  else
    die "no DISPLAY is set and xvfb-run is unavailable. Install xvfb, or run from a graphical/X-forwarded session"
  fi
fi

mkdir -p "$output_dir"

for source in "${sources[@]}"; do
  stem="$(basename "$source" .drawio)"
  output="${output_dir}/${stem}.pdf"
  prepare_export_source "$source"
  drawio_args=(-x -f pdf --crop)

  if [[ -n "$pdf_scale" ]]; then
    drawio_args+=(--scale "$pdf_scale")
  fi

  printf 'Exporting %s -> %s\n' "$(basename "$source")" "${output#"$script_dir/../"}"
  "${drawio_cmd[@]}" "${drawio_args[@]}" -o "$output" "$prepared_source"
done

printf 'Done. PDFs are in %s\n' "$output_dir"
write_tex_file
printf 'Done. LaTeX figure commands are in %s\n' "$tex_file"
