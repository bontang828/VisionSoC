# BONSAI
This project focus on developing a vision System-on-Chip (SoC) that can be used for mordern low power spatial AI compute applications. It uses a near-sensor architecture approach to reduce the sensor data movement between the sensor and the processing unit. BONSAI is a vector processor with 2D directional processing capabilities, based on a forked version of the T1 RISC-V vector processor from ChipAlliance, see [the original T1 vector processor](T1_README.md). 


## Contribution
This report introduces a novel architecture for near-sensor processing and a programming method using the standard open-source RISC-V RVV ISA to control 2D image kernels. The architecture is deployed onto an FPGA for prototyping and experimenting with simple vision kernels.

The main contributions in this research are:

- A novel use of the standard RVV ISA, without custom instructions, to perform 2D image-processing kernels and remove the directional bias of 1D vector processing. Diagram: [2D Instruction Example](fyp_diagram/selected_output/fabric_instruction_basics_v2.pdf).
- Adaptation of the 1D vector register file (VRF) into a 2D image-plane VRF, with [RVV in 1D vs 2D](fyp_diagram/selected_output/rvv_vs_t1_v4.pdf) and [2D VRF Banking Strategy](fyp_diagram/selected_output/vrf_diagonal_banking_v5_per_word.pdf), enabling similar performance in horizontal and vertical processing across the image plane with [Horizontal & Vertical Instruction Benchmark](fyp_diagram/selected_output/benchmark.pdf), and keeping image processing on chip without delegating compute to the CPU core.
- Implementation of a live camera-to-display pipeline on FPGA. Diagram: [FPGA system top](fyp_diagram/selected_output/fpga_system_top_t1style_v5.pdf).
- Optimisation of the ASIC design for FPGA deployment, fitting a configuration of 32 image-plane registers ($128\times128$ pixels each, 8-bits per pixel) with time-multiplexed computation (about 1024 $\times$ slower than the fully parallel design). Diagram: [FPGA ASIC arch](fyp_diagram/selected_output/T1_abstract_arch_fpga.pdf). Table of resources usage available in the report.
- Development of basic demonstration kernels as a proof of concept, covering both local (Sobel filtering, optical flow) and global (matrix multiplication, projection-free self-attention) context processing. Diagrams to show stages in different kernels: [Sobel](fyp_diagram/selected_output/sobel_input_ouput.pdf), [Optical Flow](fyp_diagram/selected_output/optical_flow_input_ouput.pdf), [MatMul](fyp_diagram/selected_output/matmul_8bitraw_short_steps_v3.pdf), [Projection Free Self Attention](fyp_diagram/selected_output/attention_self.pdf).
- Characterisation of the architecture's performance, including measurement and evaluation of the bottleneck in global-context operations. Diagrams: [Sobel Perf](fyp_diagram/selected_output/sobel_perf.pdf), [Optical Flow Perf](fyp_diagram/selected_output/optical_flow_perf.pdf), [MatMul Perf](fyp_diagram/selected_output/matmul_8bitraw_short_perf.pdf), [Projection Free Self Attention Perf(Simplified)](fyp_diagram/selected_output/attention_inst_perf_simp.pdf), [Projection Free Self Attention Perf(Detailed)](fyp_diagram/selected_output/attention_inst_perf_whole.pdf).





<!-- \newcommand{\figAsicDie}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/asic_die_v2}
    \caption{This shows the proposed ASIC die design placement of the CMOS sensor and the 2D vector processor on the same die. Vector instructions will stream over via the die-to-die connection and the captured image never leaves the Near-sensor Processor Die, only the computed result does.}
    \label{fig:asic_die_v2}
\end{figure}
}

\newcommand{\figAttentionInputOuput}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=0.6\textwidth
    ]{fyp_diagram/selected_output/attention_input_ouput}
    \caption{Camera Y-plane input and output for the \texttt{attention\_self} kernel. The frame on the left is split into 8$\times$8 patches(total 16$\times$16 patches), and with identity projections ($Q=XW_Q=XI=X$; $K=XW_K=XI=X$; $V=XW_V=XI=X$) each patch is used directly as its query, key, and value. The output patch is therefore a similarity-weighted average of all patches in the same frame. If a patch is most similar to itself, the output remains close to the original patch. If it is similar to other regions of the image, the output becomes a blend of those regions.}
    \label{fig:attention_input_ouput}
\end{figure}
}

\newcommand{\figAttentionSelf}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/attention_self}
    \caption{Stages in the kernel of self-attention with identity projections(Direction from left to right). Entire attention calculation executes on the fabric, from camera image to attention output. An image is 128$\times$128 pixels, with 8$\times$8 pixel per patch, we have 16$\times$16 patches in total, this produce 256 tokens each with 64 features. As 2D register is 128$\times$128 8-bits in dimension, which exceed the 256 rows needed for the 256 tokens. Hence, the 256 tokens need to split across two 2D registers(shown as top \& bottom squares) to have two 128$\times$64 shape for MatMul. As well as splitting the matrix into first half block and second half block for two pass to complete the 256$\times$256 $QK^T$ matrix output while reusing the same 2 vector registers. Therefore, there are black coloured region on the right for some stages to show the operations are disabled on the right as there are no feature present.}
    \label{fig:attention_self}
\end{figure}
}

\newcommand{\figFabricInstructionBasics}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/fabric_instruction_basics_v2}
    \caption{Instruction basics for the 2D T1 fabric. Highlighted the difference between horizontal and vertical mode execution with same RVV opcode}
    \label{fig:fabric_instruction_basics}
\end{figure}
}

\newcommand{\figFpgaSystemTop}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/fpga_system_top_t1style_v5}
    \caption{Top level FPGA system architecture for AMD Kria KV260. Highlighted with 3 major component group: Camera module, Processor System, Programmable Logic}
    \label{fig:fpga_system_top_t1style_v4}
\end{figure}
}
\newcommand{\figBenchmark}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=1.15\textwidth
    ]{fyp_diagram/selected_output/benchmark}
    \caption{A representative subset of RVV vector instructions across different flavours including arithmetic, bit comparison, elementwise \& rowwise reductions, masked, memory access, shifting, gather and logic operations. They run on the time multiplexed FPGA implementation with only 1 row of hardware processor instantiated handling 16 bytes(16 8-bits elements) at a time. The whole image plane contains 128$\times$128 elements.}
    \label{fig:benchmark}
\end{figure}
}

\newcommand{\figImageToVectorFabric}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/image_to_vector_fabric_v3}
    \caption{Demonstrate an image map to a 1D vector processor and a 2D vector processor}
    \label{fig:image_to_vector_fabric_v3}
\end{figure}
}

\newcommand{\figMatmulPerf}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/matmul_8bitraw_short_perf}
    \caption{One frame of high level 8-bits MatMul kernel FPGA execution pipeline breakdown in time stages(top bar). Detailed breakdown of 8-bits MatMul kernel with individual RVV instruction performance in time stages(bottom bar). The 128 iteration of MatMul instructions(6-12) in the bottom bar are grouped together for clarity of total execution time used per instruction group. The actual MatMul instructions would looks like a fine breakdown of 128 iterations each use a fraction of the total T1 kernel time. Kernel performing 128$\times$128 matmul at 7.5FPS @60MHz fabric, should run at 8.5FPS without camera pipeline overhead with this initial unoptimised implementation.}
    \label{fig:matmul_8bitraw_short_perf}
\end{figure}
}

\newcommand{\figMatmulSteps}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/matmul_8bitraw_short_steps_v4}
    \caption{Execution steps for matrix multiplication on the 2D vector architecture using standard RVV ISA and toggled between horizontal mode and vertical mode. This is a 3$\times$3 matrix example, real kernel runs with 128$\times$128 matrix with 8-bits elements.}
    \label{fig:matmul_8bitraw_short_steps_v3}
\end{figure}
}

\newcommand{\figOpticalFlowInputOuput}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=0.7\textwidth
    ]{fyp_diagram/selected_output/optical_flow_input_ouput}
    \caption{Stages of the optical flow kernel(left to right). Luminous plane feeds into 2D Vector processor and kernel output optical flow image. Different colour at the output image shows pixels are moving at different direction. Both hands are moving towards the center in this case.}
    \label{fig:optical_flow_input_ouput}
\end{figure}
}

\newcommand{\figAttentionInstPerfSimp}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/attention_inst_perf_simp}
    \caption{Breakdown of the self-attention kernel per stage in high level without the camera pipeline. Showing micros-seconds spent @60MHz fabric and their percentage of time consumed within the kernel. Kernel uses 60M cycles to complete and is running at 1 FPS. Performing 1 attention kernel per frame with identity weight.}
    \label{fig:attention_inst_perf_simp}
\end{figure}
}

\newcommand{\figAttentionInstPerfWhole}{%
\clearpage
\begin{figure}[H]
    \centering
    \includegraphics[
        % width=\textwidth
        angle=270,
        width=0.28\textheight,
        keepaspectratio
    ]{fyp_diagram/selected_output/attention_inst_perf_whole}
    \caption{Detailed breakdown of the self-attention kernel per stage and also per instructions in high level without the camera pipeline. Showing cycles and micros-seconds spent @60MHz fabric and their percentage of time consumed within the kernel.}
    \label{fig:attention_inst_perf_whole}
\end{figure}
\clearpage
}

\newcommand{\figOpticalFlowPerf}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/optical_flow_perf}
    \caption{One frame of high level Optical-flow kernel FPGA execution pipeline breakdown in time stages(top bar). Detailed breakdown of Optical-flow kernel with individual RVV instruction performance in time stages(bottom bar). Kernel performing optical flow at 30.0FPS(bottleneck by camera module) @60MHz fabric, in theory should run at 161FPS with this initial unoptimised implementation.}
    \label{fig:optical_flow_perf}
\end{figure}
}

\newcommand{\figRvvVsTOne}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/rvv_vs_t1_v4}
    \caption{Comparison between 1D RVV and the 2D RVV fabric in data movements when executing instructions on a grid of data. Highlighted the reduction in data round trips required between transpose on 2D RVV compared to 1D RVV.}
    \label{fig:rvv_vs_t1_v2}
\end{figure}
}

\newcommand{\figSobelInputOuput}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=0.7\textwidth
    ]{fyp_diagram/selected_output/sobel_input_ouput}
    \caption{Stages of the sobel kernel(left to right). Luminous plane feeds into 2D Vector processor and kernel output sobel filtered image. }
    \label{fig:sobel_input_ouput}
\end{figure}
}

\newcommand{\figSobelSteps}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/sobel_kernel_steps_v3}
    \caption{Sobel kernel execution steps on the 2D RVV vector architecture. Steps from top to bottom showing data movements with coloured tiles. This is an illustrated 3$\times$3 example, actual kernel runs at 128$\times$128 pixels}
    \label{fig:sobel_kernel_steps_v2}
\end{figure}
}

\newcommand{\figSobelPerf}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/sobel_perf}
    \caption{One frame of high level Sobel kernel FPGA execution pipeline breakdown in time stages(top bar). Detailed breakdown of Sobel kernel with individual RVV instruction performance in time stages(bottom bar). Kernel performing Sobel filtering at 30.0FPS(bottleneck by camera module) @60MHz fabric, in theory should run at 373FPS with this initial unoptimised implementation.}
    \label{fig:sobel_perf}
\end{figure}
}

\newcommand{\figTOneAbstractArchFpga}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/T1_abstract_arch_fpga}
    \caption{Abstracted top level FPGA adaptation of the 2D T1 architecture. Highlighted with time multiplex lane processing, PS interface wrapper and scratch-pad memory subsystem.}
    \label{fig:T1_abstract_arch_fpga}
\end{figure}
}

\newcommand{\figTOneAbstractArchRtl}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/T1_abstract_arch_rtl}
    \caption{Abstracted top level RTL of 2D T1 architecture.}
    \label{fig:T1_abstract_arch_rtl}
\end{figure}
}

\newcommand{\figTOneVsScamp}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/t1_vs_scamp5_v5}
    \caption{Comparison between the T1 fabric and SCAMP-5 in programmability. Highlighted the data control abstractions with RISC-V RVV instructions and execution in a 2D plane.}
    \label{fig:t1_vs_scamp5_v3}
\end{figure}
}

\newcommand{\figVrfDiagonalBanking}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/vrf_diagonal_banking_v3}
    \caption{Diagonal banking scheme used for the vector register file in 2D T1 memory subsystem.}
    \label{fig:vrf_diagonal_banking_v3}
\end{figure}
}

\newcommand{\figVrfDiagonalBankingVfivePerWord}{%
\begin{figure}[H]
    \centering
    \includegraphics[
        width=\textwidth
    ]{fyp_diagram/selected_output/vrf_diagonal_banking_v5_per_word}
    \caption{Diagonal banking scheme used for the vector register file in 2D T1 memory subsystem.}
    \label{fig:vrf_diagonal_banking_v5_per_word}
\end{figure}
} -->




