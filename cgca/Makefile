# Makefile -- the C tools (gcac_pack / gcac_unpack).
#
# On Windows use build.bat (Visual C++ 6); this makefile is for POSIX
# systems and any other C compiler.
#
#   make            build both tools
#   make test       build and run the round trip test against ../gcac.exe
#                   (the test itself lives outside the repository)

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS ?= -I.
LDFLAGS ?=
LDLIBS  ?= -lm

COMMON  = gca_util.o gca_crypt.o gca_codec.o gca_arc.o

all: gcac_pack gcac_unpack

gcac_pack: gca_pack.o $(COMMON)
	$(CC) $(LDFLAGS) -o $@ gca_pack.o $(COMMON) $(LDLIBS)

gcac_unpack: gca_unpack.o $(COMMON)
	$(CC) $(LDFLAGS) -o $@ gca_unpack.o $(COMMON) $(LDLIBS)

%.o: %.c gca.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f *.o gcac_pack gcac_unpack

.PHONY: all clean
