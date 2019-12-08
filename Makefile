
#CC=arm-linux-gnueabihf-gcc
#CFLAGS=-g -I/opt/vc/include -Iinclude -D_REENTRANT \
	-DVCHI_BULK_GRANULARITY=1 -DOMX_SKIP64BIT -DEGL_SERVER_DISPMANX \
	-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 \
	-D_GNU_SOURCE -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -marm \
	-Wno-multichar -Wall -Wno-unused-but-set-variable -fPIC
CFLAGS=-I/opt/vc/include -Iinclude -g
CFLAGS+=-DEGL_SERVER_DISPMANX -DHAVE_CMAKE_CONFIG -DHAVE_VMCS_CONFIG \
	-DOMX_SKIP64BIT -DTV_SUPPORTED_MODE_NO_DEPRECATED -DUSE_VCHIQ_ARM \
	-DVCHI_BULK_ALIGN=1 -DVCHI_BULK_GRANULARITY=1 -D_FILE_OFFSET_BITS=64 \
	-D_GNU_SOURCE -D_HAVE_SBRK -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE \
	-D_REENTRANT -D__VIDEOCORE4__ -DbrcmEGL_EXPORTS
CFLAGS+=-I/home/pi/src/userland/build/inc \
	-I/home/pi/src/userland/host_applications/framework \
	-I/home/pi/src/userland \
	-I/home/pi/src/userland/interface/vcos/pthreads \
	-I/home/pi/src/userland/interface/vmcs_host/linux \
	-I/home/pi/src/userland/interface/vmcs_host \
	-I/home/pi/src/userland/interface/vmcs_host/khronos \
	-I/home/pi/src/userland/interface/khronos/include \
	-I/home/pi/src/userland/build \
	-I/home/pi/src/userland/interface/vchiq_arm \
	-I/home/pi/src/userland/host_support/include  
CFLAGS+=-Wno-multichar -Wall -Wno-unused-but-set-variable -fPIC -fPIC
LDFLAGS=-L/opt/vc/lib -Wl,-rpath /opt/vc/lib -lmmal -lmmal_core -lmmal_components -lmmal_vc_client \
	-lmmal_util -lvcos -lbcm_host -lpthread

#-DUSE_VCHIQ_ARM -DVCHI_BULK_ALIGN=1 

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