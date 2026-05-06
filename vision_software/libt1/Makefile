CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -Wall -Wextra -Werror -fPIC -g
LDFLAGS ?=

LIBT1_SRCS := libt1.c
LIBT1_OBJS := $(LIBT1_SRCS:.c=.o)
TESTS := test/smoke test/ddr_roundtrip test/port_grid_vadd test/dma_loopback test/vert_lsu

all: libt1.a libt1.so $(TESTS)

libt1.a: $(LIBT1_OBJS)
	$(AR) rcs $@ $^

libt1.so: $(LIBT1_OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

test/%: test/%.c libt1.a
	$(CC) $(CFLAGS) -I. -I../visionsoc_main -o $@ $< libt1.a

clean:
	$(RM) $(LIBT1_OBJS) libt1.a libt1.so $(TESTS)

install: libt1.a libt1.so
	install -m 0644 libt1.h /usr/local/include/
	install -m 0644 libt1_regs.h /usr/local/include/
	install -m 0644 weights_format.h /usr/local/include/
	install -m 0644 libt1.a /usr/local/lib/
	install -m 0755 libt1.so /usr/local/lib/
	ldconfig

.PHONY: all clean install
