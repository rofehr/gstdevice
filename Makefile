#
# Makefile for a Video Disk Recorder plugin
#
# Based on: https://github.com/rofehr/gstreamerdevice/blob/kms/Makefile
#

# The official name of this plugin.
# This name will be used in the '-P...' option of VDR to load the plugin.
PLUGIN = gstreamer

### The version number of this plugin (taken from the main source file):
VERSION = $(shell grep 'define PLUGIN_VERSION' gstreamer.h | awk '{ print $$3 }' | sed -e 's/[";]//g')

### The directory environment:
# Resolve all VDR build variables via vdr.pc.
# If VDRDIR is set, use that path's vdr.pc; otherwise fall back to
# the pkg-config search path (works both in-tree and installed).
PKGCFG = $(if $(VDRDIR),$(shell pkg-config --variable=$(1) $(VDRDIR)/vdr.pc),\
                         $(shell PKG_CONFIG_PATH="$$PKG_CONFIG_PATH:../../.." pkg-config --variable=$(1) vdr))

LIBDIR  = $(call PKGCFG,libdir)
LOCDIR  = $(call PKGCFG,locdir)
PLGCFG  = $(call PKGCFG,plgcfg)

TMPDIR ?= /tmp

### The compiler options (from vdr.pc):
export CFLAGS   = $(call PKGCFG,cflags)
export CXXFLAGS = $(call PKGCFG,cxxflags) -std=c++17

### The version number of VDR's plugin API (used as .so suffix):
APIVERSION = $(call PKGCFG,apiversion)

### Allow user-defined options to overwrite defaults:
-include $(PLGCFG)

### Distribution archive names:
ARCHIVE = $(PLUGIN)-$(VERSION)
PACKAGE = vdr-$(ARCHIVE)

### The name of the shared object file:
SOFILE = libvdr-$(PLUGIN).so

### ---------------------------------------------------------------
### Includes and Defines
### ---------------------------------------------------------------

# GStreamer packages required at compile time
GST_PKGS = gstreamer-1.0 \
           gstreamer-app-1.0 \
           gstreamer-video-1.0 \
           gstreamer-audio-1.0

INCLUDES += $(shell pkg-config --cflags $(GST_PKGS))

DEFINES  += -DPLUGIN_NAME_I18N='"$(PLUGIN)"'

### ---------------------------------------------------------------
### Object files
### ---------------------------------------------------------------
OBJS = gstreamer.o \
       gstdevice.o \
       gstosd.o    \
       setup.o

### ---------------------------------------------------------------
### Main target
### ---------------------------------------------------------------
all: $(SOFILE) i18n

### ---------------------------------------------------------------
### Implicit compile rule
### ---------------------------------------------------------------
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $(DEFINES) $(INCLUDES) -o $@ $<

### ---------------------------------------------------------------
### Automatic header dependencies
### ---------------------------------------------------------------
MAKEDEP = $(CXX) -MM -MG
DEPFILE = .dependencies

$(DEPFILE): Makefile
	@$(MAKEDEP) $(CXXFLAGS) $(DEFINES) $(INCLUDES) $(OBJS:%.o=%.cpp) > $@

-include $(DEPFILE)

### ---------------------------------------------------------------
### Internationalization (I18N)
### ---------------------------------------------------------------
PODIR    = po
I18Npo   = $(wildcard $(PODIR)/*.po)
I18Nmo   = $(addsuffix .mo, $(foreach file, $(I18Npo), $(basename $(file))))
I18Nmsgs = $(addprefix $(DESTDIR)$(LOCDIR)/, \
             $(addsuffix /LC_MESSAGES/vdr-$(PLUGIN).mo, \
               $(notdir $(foreach file, $(I18Npo), $(basename $(file))))))
I18Npot  = $(PODIR)/$(PLUGIN).pot

%.mo: %.po
	msgfmt -c -o $@ $<

$(I18Npot): $(wildcard *.cpp *.h)
	xgettext -C -cTRANSLATORS --no-wrap --no-location \
	  -k -ktr -ktrNOOP \
	  --package-name=vdr-$(PLUGIN) \
	  --package-version=$(VERSION) \
	  --msgid-bugs-address='<see README>' \
	  -o $@ `ls $^`

%.po: $(I18Npot)
	msgmerge -U --no-wrap --no-location --backup=none -q -N $@ $<
	@touch $@

$(I18Nmsgs): $(DESTDIR)$(LOCDIR)/%/LC_MESSAGES/vdr-$(PLUGIN).mo: $(PODIR)/%.mo
	install -D -m644 $< $@

.PHONY: i18n
i18n: $(I18Nmo) $(I18Npot)

install-i18n: $(I18Nmsgs)

### ---------------------------------------------------------------
### Link
### GStreamer libs resolved via pkg-config; X11/xcb not needed here
### since we use autovideosink / vaapisink instead of direct X output
### ---------------------------------------------------------------
GST_LIBS = $(shell pkg-config --libs $(GST_PKGS))

$(SOFILE): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -shared $(OBJS) \
	  $(GST_LIBS) \
	  -lgobject-2.0 -lglib-2.0 \
	  -o $@

### ---------------------------------------------------------------
### Install
### The .so is installed with the VDR API version suffix, as required
### by VDR's plugin loader: libvdr-<plugin>.so.<apiversion>
### ---------------------------------------------------------------
install-lib: $(SOFILE)
	install -D $^ $(DESTDIR)$(LIBDIR)/$^.$(APIVERSION)

install: install-lib install-i18n

### ---------------------------------------------------------------
### Distribution tarball
### ---------------------------------------------------------------
dist: $(I18Npo) clean
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@mkdir $(TMPDIR)/$(ARCHIVE)
	@cp -a * $(TMPDIR)/$(ARCHIVE)
	@tar czf $(PACKAGE).tgz -C $(TMPDIR) $(ARCHIVE)
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@echo Distribution package created as $(PACKAGE).tgz

### ---------------------------------------------------------------
### Clean
### ---------------------------------------------------------------
clean:
	@-rm -f $(PODIR)/*.mo $(PODIR)/*.pot
	@-rm -f $(OBJS) $(DEPFILE) *.so *.tgz core* *~

.PHONY: all install install-lib install-i18n dist clean
