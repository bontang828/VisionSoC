CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -g

DRM_CFLAGS := $(shell if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libdrm; then pkg-config --cflags libdrm; else echo -I/usr/include/libdrm; fi)
DRM_LIBS := $(shell if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libdrm; then pkg-config --libs libdrm; else echo -ldrm; fi)

CFLAGS += -I../libt1 $(DRM_CFLAGS)
LDLIBS ?= ../libt1/libt1.a $(DRM_LIBS)

SRCS := main.c camera.c display.c cnn2d_decoder.c
OBJS := $(SRCS:.c=.o)

all: visionsoc_main

visionsoc_main: $(OBJS) ../libt1/libt1.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

# Perf binaries: main_perf is a stage-instrumented twin of main.c (full
# camera+display loop, one CSV row per frame). The *_perf binaries are
# standalone T1-instrumented drivers for individual kernels.
visionsoc_main_perf: main_perf.o camera.o display.o cnn2d_decoder.o ../libt1/libt1.a
	$(CC) $(CFLAGS) -o $@ main_perf.o camera.o display.o cnn2d_decoder.o $(LDLIBS)

# Capture binary: twin of main.c that writes 4 PPM snapshots per capture
# frame (camera Y, camera color, T1 output, display framebuffer) for the
# report. Same camera+display+T1 path as main.c, just with snapshot sites
# inside the per-frame loop.
visionsoc_main_capture: main_capture.o camera.o display.o cnn2d_decoder.o ../libt1/libt1.a
	$(CC) $(CFLAGS) -o $@ main_capture.o camera.o display.o cnn2d_decoder.o $(LDLIBS)

sobel_perf: sobel_perf.o ../libt1/libt1.a
	$(CC) $(CFLAGS) -o $@ sobel_perf.o $(LDLIBS)

optical_flow_perf: optical_flow_perf.o ../libt1/libt1.a
	$(CC) $(CFLAGS) -o $@ optical_flow_perf.o $(LDLIBS)

matmul_8bitraw_short_perf: matmul_8bitraw_short_perf.o ../libt1/libt1.a
	$(CC) $(CFLAGS) -o $@ matmul_8bitraw_short_perf.o $(LDLIBS)

display_smoke: display_smoke.o display.o
	$(CC) $(CFLAGS) -o $@ display_smoke.o display.o $(DRM_LIBS)

../libt1/libt1.a:
	$(MAKE) -C ../libt1 libt1.a

kernels/grid_vadd.h: kernels/grid_vadd.S
	../libt1/build_kernel.sh $< $@ grid_vadd

kernels/frame_passthrough.h: kernels/frame_passthrough.S
	../libt1/build_kernel.sh $< $@ frame_passthrough

kernels/sobel.h: kernels/sobel.S
	../libt1/build_kernel.sh $< $@ sobel

kernels/cnn2d_decoder_primitives.h: kernels/cnn2d_decoder_primitives.S
	../libt1/build_kernel.sh $< $@ cnn2d_decoder_primitives

kernels/optical_flow.h: kernels/optical_flow.S
	../libt1/build_kernel.sh $< $@ optical_flow

kernels/flow_color_rgb565.h: kernels/flow_color_rgb565.S
	../libt1/build_kernel.sh $< $@ flow_color_rgb565

kernels/matmul.h: kernels/matmul.S
	../libt1/build_kernel.sh $< $@ matmul

kernels/matmul_8bitraw.h: kernels/matmul_8bitraw.S
	../libt1/build_kernel.sh $< $@ matmul_8bitraw

kernels/matmul_8bitraw_short.h: kernels/matmul_8bitraw_short.S
	../libt1/build_kernel.sh $< $@ matmul_8bitraw_short

cnn2d_decoder.o: kernels/cnn2d_decoder_primitives.h
# main.o transitively includes whichever <name>_select.h the active_kernel.h
# points at, plus the shared dispatcher. List every potential kernel + select
# header so a change to any of them triggers a rebuild.
main.o: kernels/active_kernel.h kernels/active_kernel_dispatcher.h \
        kernels/frame_passthrough_select.h kernels/frame_passthrough.h \
        kernels/sobel_select.h            kernels/sobel.h \
        kernels/optical_flow_select.h     kernels/optical_flow.h \
        kernels/flow_color_rgb565_dispatch.h kernels/flow_color_rgb565.h \
        kernels/matmul_select.h           kernels/matmul.h \
        kernels/matmul_8bitraw_select.h   kernels/matmul_8bitraw.h \
        kernels/matmul_8bitraw_short_select.h kernels/matmul_8bitraw_short.h \
        kernels/matmul_weights.h

main_perf.o: kernels/active_kernel.h kernels/active_kernel_dispatcher.h \
             kernels/frame_passthrough_select.h kernels/frame_passthrough.h \
             kernels/sobel_select.h            kernels/sobel.h \
             kernels/optical_flow_select.h     kernels/optical_flow.h \
             kernels/flow_color_rgb565_dispatch.h kernels/flow_color_rgb565.h \
             kernels/matmul_select.h           kernels/matmul.h \
             kernels/matmul_8bitraw_select.h   kernels/matmul_8bitraw.h \
             kernels/matmul_8bitraw_short_select.h kernels/matmul_8bitraw_short.h \
             kernels/matmul_weights.h

main_capture.o: kernels/active_kernel.h kernels/active_kernel_dispatcher.h \
                kernels/frame_passthrough_select.h kernels/frame_passthrough.h \
                kernels/sobel_select.h            kernels/sobel.h \
                kernels/optical_flow_select.h     kernels/optical_flow.h \
                kernels/flow_color_rgb565_dispatch.h kernels/flow_color_rgb565.h \
                kernels/matmul_select.h           kernels/matmul.h \
                kernels/matmul_8bitraw_select.h   kernels/matmul_8bitraw.h \
                kernels/matmul_8bitraw_short_select.h kernels/matmul_8bitraw_short.h \
                kernels/matmul_weights.h

sobel_perf.o: kernels/sobel.h

optical_flow_perf.o: kernels/optical_flow.h

matmul_8bitraw_short_perf.o: kernels/matmul_8bitraw_short.h kernels/matmul_weights.h

kernels: kernels/grid_vadd.h kernels/frame_passthrough.h kernels/sobel.h kernels/cnn2d_decoder_primitives.h kernels/optical_flow.h kernels/flow_color_rgb565.h kernels/matmul.h kernels/matmul_8bitraw.h kernels/matmul_8bitraw_short.h

clean:
	$(RM) $(OBJS) main_perf.o main_capture.o sobel_perf.o optical_flow_perf.o \
		matmul_8bitraw_short_perf.o display_smoke.o \
		visionsoc_main visionsoc_main_perf visionsoc_main_capture \
		sobel_perf optical_flow_perf \
		matmul_8bitraw_short_perf display_smoke

.PHONY: all clean kernels
