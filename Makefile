# Makefile – vdr-plugin-gstreamer v0.2.0
# H.264 / H.265, VA-API, AAC, OSD Setup menu

PLUGIN = gstreamer

VDRDIR      ?= /usr/include/vdr
PKG_CONFIG  ?= pkg-config

# GStreamer packages needed
GST_PKGS = gstreamer-1.0 \
           gstreamer-app-1.0 \
           gstreamer-video-1.0 \
           gstreamer-audio-1.0

GSTREAMER_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(GST_PKGS))
GSTREAMER_LIBS   := $(shell $(PKG_CONFIG) --libs   $(GST_PKGS))

CXX      ?= g++
CXXFLAGS  = -std=c++17 -O2 -Wall -Wextra -fPIC \
            -I$(VDRDIR) \
            $(GSTREAMER_CFLAGS) \
            -DPLUGIN_NAME_I18N='"$(PLUGIN)"'

LDFLAGS   = $(GSTREAMER_LIBS) -shared

SRCS = gstreamer.cpp \
       gstdevice.cpp \
       gstosd.cpp   \
       setup.cpp

OBJS = $(SRCS:.cpp=.o)

# ---- Targets ----
all: libvdr-$(PLUGIN).so

libvdr-$(PLUGIN).so: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^
	@echo "Built: $@"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

install: libvdr-$(PLUGIN).so
	install -D -m 644 $< $(DESTDIR)/usr/lib/vdr/plugins/$<

clean:
	@rm -f $(OBJS) libvdr-$(PLUGIN).so

# ---- Header dependencies ----
gstreamer.o:  gstreamer.h config.h gstdevice.h gstosd.h setup.h
gstdevice.o:  gstdevice.h gstreamer.h config.h gstosd.h tsparser.h
gstosd.o:     gstosd.h config.h
setup.o:      setup.h config.h gstosd.h

.PHONY: all install clean
