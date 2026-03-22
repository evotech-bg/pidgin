CC = gcc
CFLAGS = -O2 -Wall -Wextra -fPIC -Iinclude -Isrc
LDFLAGS = -lpthread

# macOS: detect Homebrew paths (must come before brotli detection)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
  ifneq ($(BREW_PREFIX),)
    CFLAGS += -I$(BREW_PREFIX)/include
    LDFLAGS += -L$(BREW_PREFIX)/lib
  endif
  SOEXT = dylib
  SOFLAGS = -dynamiclib -install_name @rpath/libpidgin.dylib
else
  SOEXT = so
  SOFLAGS = -shared
endif

# Detect brotli
BROTLI_CHECK := $(shell pkg-config --exists libbrotlienc libbrotlidec 2>/dev/null && echo yes || echo no)
ifeq ($(BROTLI_CHECK),yes)
  BROTLI_CFLAGS := $(shell pkg-config --cflags libbrotlienc libbrotlidec)
  BROTLI_LDFLAGS := $(shell pkg-config --libs libbrotlienc libbrotlidec)
  CFLAGS += $(BROTLI_CFLAGS)
  LDFLAGS += $(BROTLI_LDFLAGS)
else
  # Try direct linking (with Homebrew paths if available)
  BROTLI_DIRECT := $(shell echo 'int main(){return 0;}' | $(CC) -x c - -L$(BREW_PREFIX)/lib -lbrotlienc -lbrotlidec -o /dev/null 2>/dev/null && echo yes || echo no)
  ifeq ($(BROTLI_DIRECT),yes)
    LDFLAGS += -lbrotlienc -lbrotlidec
  else
    $(info [INFO] brotli not found — building without compression support)
    CFLAGS += -DPIDGIN_NO_BROTLI
  endif
endif

.PHONY: all clean test

all: libpidgin.$(SOEXT) libpidgin.a

libpidgin.$(SOEXT): src/pidgin.c src/cjson.h include/pidgin.h
	$(CC) $(CFLAGS) $(SOFLAGS) -o $@ src/pidgin.c $(LDFLAGS)

libpidgin.a: src/pidgin.c src/cjson.h include/pidgin.h
	$(CC) $(CFLAGS) -c -o pidgin.o src/pidgin.c
	ar rcs $@ pidgin.o
	rm -f pidgin.o

test: tests/test_pidgin.c libpidgin.$(SOEXT)
	$(CC) $(CFLAGS) -o test_pidgin tests/test_pidgin.c -L. -lpidgin $(LDFLAGS) -Wl,-rpath,.
	./test_pidgin

clean:
	rm -f libpidgin.so libpidgin.dylib libpidgin.a *.o test_pidgin
