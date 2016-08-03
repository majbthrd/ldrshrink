ifeq ($(OS),Windows_NT)
	EXE_SUFFIX = .exe
endif
CFLAGS += -O3

all: ldrshrink$(EXE_SUFFIX)

ldrshrink$(EXE_SUFFIX): ldrshrink.c Makefile
	gcc $(CFLAGS) ldrshrink.c -o $@
	strip $@

clean:
	rm -f ldrshrink$(EXE_SUFFIX)
