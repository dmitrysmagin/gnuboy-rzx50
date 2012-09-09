
prefix = 
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin

CC = mipsel-linux-gcc
LD = $(CC)
AS = $(CC)

CFLAGS = -O3 -march=mips32 -mtune=r4600 -msoft-float -fomit-frame-pointer -ffast-math \
	-falign-functions -falign-loops -falign-labels -falign-jumps -fno-builtin -fno-common \
	-D_GNU_SOURCE=1 -DIS_LITTLE_ENDIAN

LDFLAGS = $(CFLAGS) -s
ASFLAGS = $(CFLAGS)

TARGETS =  sdlgnuboy.dge

ASM_OBJS =

SYS_DEFS = -DHAVE_CONFIG_H -DIS_LITTLE_ENDIAN  -DIS_LINUX
SYS_OBJS = sys/nix/nix.o $(ASM_OBJS)
SYS_INCS = -I./sys/nix

SDL_OBJS = sys/sdl/sdl.o sys/sdl/keymap.o sys/sdl/scaler.o #sys/oss/oss.o
SDL_LIBS =  -lSDL
SDL_CFLAGS = -D_GNU_SOURCE=1

all: $(TARGETS)

include Rules

$(TARGETS): $(OBJS) $(SYS_OBJS) $(SDL_OBJS)
	$(LD) $(LDFLAGS) $(OBJS) $(SYS_OBJS) $(SDL_OBJS) -o $@ $(SDL_LIBS)


clean:
	rm -f *gnuboy gmon.out *.o sys/*.o sys/*/*.o asm/*/*.o





