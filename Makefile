#
# Makefile for a Video Disk Recorder plugin
#
# Based on: https://github.com/rofehr/gstreamerdevice/blob/kms/Makefile
# Adapted for: vdr-plugin-gstreamer v0.3.0 (VDR >= 2.7.7, C++17)
#

# The official plugin name (used in '-P gstreamer' VDR option)
PLUGIN = gstreamer

### Version is taken from the main header file
VERSION = $(shell grep 'define PLUGIN_VERSION' gstreamer.h | awk '{ print $$3 }' | sed -e 's/[";]//g')

### VDR build variable resolution via vdr.pc
# If VDRDIR is set, use that vdr.pc; otherwise fall back to pkg-config search path.
PKGCFG = $(if $(VDRDIR),$(shell pkg-config --variable=$(1) $(VDRDIR)/vdr.pc),\
                         $(shell PKG_CONFIG_PATH="$$PKG_CONFIG_PATH:../../.." pkg-config --variable=$(1) vdr))

LIBDIR     = $(call PKGCFG,libdir)
LOCDIR     = $(call PKGCFG,locdir)
PLGCFG     = $(call PKGCFG,plgcfg)

TMPDIR    ?= /tmp

### Compiler flags come from vdr.pc (includes VDR APIVERSION defines etc.)
export CFLAGS   = $(call PKGCFG,cflags)
export CXXFLAGS = $(call PKGCFG,cxxflags) -std=c++17

### VDR API version (appended to .so on install)
APIVERSION = $(call PKGCFG,apiversion)

### Allow user-defined options to override defaults
-include $(PLGCFG)

### Archive / package names
ARCHIVE = $(PLUGIN)-$(VERSION)
PACKAGE = vdr-$(ARCHIVE)

### Shared object filename
SOFILE = libvdr-$(PLUGIN).so

### -------------------------------------------------------------------
### Includes and defines
### -------------------------------------------------------------------

GST_PKGS = gstreamer-1.0 \
           gstreamer-app-1.0 \
           gstreamer-video-1.0 \
           gstreamer-audio-1.0

INCLUDES += $(shell pkg-config --cflags $(GST_PKGS))
DEFINES  += -DPLUGIN_NAME_I18N='"$(PLUGIN)"'

### -------------------------------------------------------------------
### Object files
### -------------------------------------------------------------------
OBJS = gstreamer.o \
       gstdevice.o \
       gstosd.o    \
       setup.o

### -------------------------------------------------------------------
### Main target
### -------------------------------------------------------------------
all: $(SOFILE) i18n

### -------------------------------------------------------------------
### Compile rule
### -------------------------------------------------------------------
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $(DEFINES) $(INCLUDES) -o $@ $<

### -------------------------------------------------------------------
### Automatic header dependencies
### -------------------------------------------------------------------
MAKEDEP = $(CXX) -MM -MG
DEPFILE = .dependencies

$(DEPFILE): Makefile
	@$(MAKEDEP) $(CXXFLAGS) $(DEFINES) $(INCLUDES) $(OBJS:%.o=%.cpp) > $@

-include $(DEPFILE)

### -------------------------------------------------------------------
### Internationalization (I18N)
### -------------------------------------------------------------------
PODIR    = po
I18Npo   = $(wildcard $(PODIR)/*.po)
I18Nmo   = $(addsuffix .mo,$(foreach file,$(I18Npo),$(basename $(file))))
I18Nmsgs = $(addprefix $(DESTDIR)$(LOCDIR)/,\
             $(addsuffix /LC_MESSAGES/vdr-$(PLUGIN).mo,\
               $(notdir $(foreach file,$(I18Npo),$(basename $(file))))))
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

### -------------------------------------------------------------------
### Link
### GStreamer libs resolved via pkg-config.
### -------------------------------------------------------------------
GST_LIBS = $(shell pkg-config --libs $(GST_PKGS))

$(SOFILE): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -shared $(OBJS) \
	       $(GST_LIBS) \
	       -lgobject-2.0 -lglib-2.0 \
	       -o $@

### -------------------------------------------------------------------
### Install
### The plugin .so is installed with the VDR API version suffix, as
### required by VDR's plugin loader:
###   /usr/lib/vdr/plugins/libvdr-gstreamer.so.<apiversion>
### -------------------------------------------------------------------
install-lib: $(SOFILE)
	install -D $^ $(DESTDIR)$(LIBDIR)/$^.$(APIVERSION)

install: install-lib install-i18n

### -------------------------------------------------------------------
### Distribution tarball
### -------------------------------------------------------------------
dist: $(I18Npo) clean
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@mkdir $(TMPDIR)/$(ARCHIVE)
	@cp -a * $(TMPDIR)/$(ARCHIVE)
	@tar czf $(PACKAGE).tgz -C $(TMPDIR) $(ARCHIVE)
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@echo Distribution package created as $(PACKAGE).tgz

### -------------------------------------------------------------------
### Clean
### -------------------------------------------------------------------
clean:
	@-rm -f $(PODIR)/*.mo $(PODIR)/*.pot
	@-rm -f $(OBJS) $(DEPFILE) *.so *.tgz core* *~

.PHONY: all install install-lib install-i18n dist clean
