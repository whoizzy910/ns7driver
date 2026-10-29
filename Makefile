# NS7 userspace driver
#   make            build ns7d   (macOS: brew install libusb pkg-config)
#   make test       run the unit tests (no hardware needed)
UNAME := $(shell uname)
# libusb from pkg-config, or pass LIBUSB_CFLAGS / LIBUSB_LIBS (static build for the .pkg)
LIBUSB_CFLAGS ?= $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS   ?= $(shell pkg-config --libs libusb-1.0)
CFLAGS += -O2 -Wall -Wextra -std=gnu11 $(LIBUSB_CFLAGS) $(CFLAGS_EXTRA)
LIBS   := $(LIBUSB_LIBS) -lpthread -lm
SRC    := src/ns7d.c src/ns7_usb.c src/ns7_midi.c src/ns7_wav.c src/ns7_serve.c

ifeq ($(UNAME),Darwin)
  FRAMEWORKS := -framework CoreMIDI -framework CoreFoundation
else
  LIBS += -lrt
endif
LIBS += $(FRAMEWORKS)

ns7d: $(SRC) src/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)

TEST_EXTRA :=
ifdef OZZY
  TEST_EXTRA := -DHAVE_OZZY $(OZZY)/common/devices/ploytec/ploytec_codec.c
endif

test: tests/test_proto.c src/*.h src/ns7_midi.c src/ns7_wav.c
	$(CC) -O2 -Wall -Wextra -std=gnu11 -o tests/test_proto tests/test_proto.c src/ns7_wav.c \
	    src/ns7_midi.c $(TEST_EXTRA) -lm $(FRAMEWORKS)
	./tests/test_proto
	$(CC) -O2 -Wall -Wextra -std=gnu11 -o tests/test_timeline tests/test_timeline.c src/ns7_serve.c \
	    src/ns7_usb.c src/ns7_midi.c $(LIBUSB_CFLAGS) $(LIBS)
	./tests/test_timeline

clean:
	rm -f ns7d tests/test_proto tests/test_timeline

.PHONY: test clean
