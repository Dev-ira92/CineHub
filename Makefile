# Cine Hub - PS5 payload (SDL2 + libcurl), run through websrv.
# Recipes use ">" instead of TAB so copy-paste cannot break them.

TARGET := cinehub.elf
SRCDIR := src
BUILD  := build

SRCS := $(SRCDIR)/main.c $(SRCDIR)/movies.c $(SRCDIR)/common.c \
        $(SRCDIR)/ui.c $(SRCDIR)/stb_impl.c $(SRCDIR)/shape.c \
        $(SRCDIR)/text.c $(SRCDIR)/player.c

WARN        := -Wall -Wextra -Wno-unused-parameter
BASE_CFLAGS := $(WARN) -O2 -I$(SRCDIR)

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

.RECIPEPREFIX := >

ifdef PS5_PAYLOAD_SDK
# Same package list as ComicReader (known to link); static libcurl
# pulls its dependencies through these .pc files.
# FFmpeg (staticki) se gradi workflow-om "Build FFmpeg for PS5".
FFMPEG      ?= ffmpeg-prefix
PS5_PKGS    := sdl2 libarchive libwebp libcurl libxml-2.0
BASE_CFLAGS += -DCURL_STATICLIB -I$(FFMPEG)/include
PS5_CFLAGS  := $(shell $(PKG_CONFIG) --cflags $(PS5_PKGS))
PS5_LIBS    := $(shell $(PKG_CONFIG) --libs $(PS5_PKGS))

# Zvuk: stub biblioteke SceAudioOut iz SDK-a. Dodaje se samo ako ga linker
# stvarno nadje, pa bez njega program ostaje bez zvuka umjesto da ne linka.
AUDIO_LIB   := $(shell printf 'int main(void){return 0;}\n' > /tmp/cine_probe.c && \
                 $(CC) /tmp/cine_probe.c -nodefaultlibs -lc -lkernel_sys \
                 -lSceLibcInternal -lSceAudioOut -o /tmp/cine_probe.elf \
                 >/dev/null 2>&1 && echo -lSceAudioOut)
$(info audio library: $(if $(AUDIO_LIB),$(AUDIO_LIB),NOT FOUND - no sound))

CFLAGS += $(BASE_CFLAGS) $(PS5_CFLAGS)
# Without this the process dies with SIGSYS (see ComicReader SETUP.md).
LDADD  += -nodefaultlibs -L$(FFMPEG)/lib -lavformat -lavcodec -lswscale \
          -lswresample -lavutil $(PS5_LIBS) -lz -lSDL2main -lm \
          -lc -lkernel_sys -lSceLibcInternal -lSceNet $(AUDIO_LIB)

OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(SRCS))

all: $(TARGET)

$(BUILD):
> @mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
> $(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
> $(CC) -o $@ $(OBJS) $(LDADD)

.PHONY: all
endif

clean:
> rm -rf $(BUILD) $(TARGET)

.PHONY: clean
