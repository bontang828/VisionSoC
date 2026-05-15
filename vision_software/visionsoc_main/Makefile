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

display_smoke: display_smoke.o display.o
	$(CC) $(CFLAGS) -o $@ display_smoke.o display.o $(DRM_LIBS)

../libt1/libt1.a:
	$(MAKE) -C ../libt1 libt1.a

kernels/grid_vadd.h: kernels/grid_vadd.S
	../libt1/build_kernel.sh $< $@ grid_vadd

kernels/frame_passthrough.h: kernels/frame_passthrough.S
	../libt1/build_kernel.sh $< $@ frame_passthrough

kernels/cnn2d_decoder_primitives.h: kernels/cnn2d_decoder_primitives.S
	../libt1/build_kernel.sh $< $@ cnn2d_decoder_primitives

cnn2d_decoder.o: kernels/cnn2d_decoder_primitives.h
main.o: kernels/frame_passthrough.h

kernels: kernels/grid_vadd.h kernels/frame_passthrough.h kernels/cnn2d_decoder_primitives.h

clean:
	$(RM) $(OBJS) display_smoke.o visionsoc_main display_smoke

.PHONY: all clean kernels
