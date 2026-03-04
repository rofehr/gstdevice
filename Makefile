# Makefile – vdr-plugin-gstreamer v0.2.0
# H.264 / H.265, VA-API, AAC, OSD Setup menu

PLUGIN = gstreamer

PKGCFG = $(if $(VDRDIR),$(shell pkg-config --variable=$(1) $(VDRDIR)/vdr.pc),$(shell PKG_CONFIG_PATH="$$PKG_CONFIG_PATH:../../.." pkg-config --variable=$(1) vdr))

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
