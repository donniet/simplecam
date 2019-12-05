
CC=arm-linux-gnueabihf-gcc
CFLAGS=-g -I/opt/vc/include -Iinclude -D_REENTRANT -DUSE_VCHIQ_ARM -DVCHI_BULK_ALIGN=1 \
	-DVCHI_BULK_GRANULARITY=1 -DOMX_SKIP64BIT -DEGL_SERVER_DISPMANX \
	-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 \
	-D_GNU_SOURCE -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -marm \
	-Wno-multichar -Wall -Wno-unused-but-set-variable -fPIC
LDFLAGS=-L/opt/vc/lib -Wl,-rpath /opt/vc/lib -lmmal -lmmal_core -lmmal_components -lmmal_vc_client \
	-lmmal_util -lvcos -lbcm_host -lpthread

SRCS=$(wildcard src/*.c)
OBJS=$(patsubst %.c,%.o,${SRCS})

simplecam: main.o ${OBJS}
	${CC} ${LDFLAGS} -o $@ $<

main.o: main.c
	${CC} ${CFLAGS} -c -o $@ $<

src/%.o: src/%.c
	${CC} ${CFLAGS} -c -o $@ $<

.PHONY: clean

clean:
	rm -f simplecam main.o ${OBJS}