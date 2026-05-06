CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -g

DRM_CFLAGS := $(shell if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libdrm; then pkg-config --cflags libdrm; else echo -I/usr/include/libdrm; fi)
DRM_LIBS := $(shell if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libdrm; then pkg-config --libs libdrm; else echo -ldrm; fi)

CFLAGS += -I../libt1 $(DRM_CFLAGS)
LDLIBS ?= ../libt1/libt1.a $(DRM_LIBS)

SRCS := main.c camera.c display.c
OBJS := $(SRCS:.c=.o)

all: visionsoc_main

visionsoc_main: $(OBJS) ../libt1/libt1.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

../libt1/libt1.a:
	$(MAKE) -C ../libt1 libt1.a

kernels/grid_vadd.h: kernels/grid_vadd.S
	../libt1/build_kernel.sh $< $@ grid_vadd

kernels: kernels/grid_vadd.h

clean:
	$(RM) $(OBJS) visionsoc_main

.PHONY: all clean kernels
