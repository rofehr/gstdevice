#
# Makefile for vdr-plugin-gstreamer
#
# Style based on: github.com/rofehr/gstreamerdevice (kms branch)
# Requires: VDR >= 2.6.0, GStreamer >= 1.20, C++17
#

PLUGIN  = gstreamer
VERSION = $(shell grep 'PLUGIN_VERSION' gstreamer.h | \
            awk '{print $$3}' | sed 's/[";]//g')

### -- VDR paths via vdr.pc --------------------------------------------------
# Set VDRDIR to the directory containing vdr.pc, or leave unset to use
# the system pkg-config search path.
#PKGCFG = $(if $(VDRDIR),\
#    $(shell pkg-config --variable=$(1) $(VDRDIR)/vdr.pc),\
#    $(shell PKG_CONFIG_PATH="$$PKG_CONFIG_PATH:../../.." \
#            pkg-config --variable=$(1) vdr))

#PKGCFG = $(if $(VDRDIR),$(shell pkg-config --variable=$(1) $(VDRDIR)/vdr.pc),$(shell PKG_CONFIG_PATH="$$PKG_CONFIG_PATH:../../.." pkg-config --variable=$(1) vdr))
LIBDIR     = $(call PKGCFG,libdir)
LOCDIR     = $(call PKGCFG,locdir)
PLGCFG     = $(call PKGCFG,plgcfg)
APIVERSION = $(call PKGCFG,apiversion)

TMPDIR    ?= /tmp

### -- Compiler flags from vdr.pc --------------------------------------------
export CFLAGS   = $(call PKGCFG,cflags)
export CXXFLAGS = $(call PKGCFG,cxxflags) -std=c++17 -fPIC 

APIVERSION = $(call PKGCFG,apiversion)

### -- User overrides (plgcfg) -----------------------------------------------
-include $(PLGCFG)

### -- GStreamer packages -----------------------------------------------------
GST_PKGS = gstreamer-1.0        \
           gstreamer-app-1.0    \
           gstreamer-video-1.0  \
           gstreamer-audio-1.0

INCLUDES += $(shell pkg-config --cflags $(GST_PKGS))
GST_LIBS  = $(shell pkg-config --libs   $(GST_PKGS))

DEFINES  += -DPLUGIN_NAME_I18N='"$(PLUGIN)"'

### -- Object files ----------------------------------------------------------
OBJS = gstreamer.o \
       gstdevice.o \
       gstosd.o    \
       setup.o

### -- Archive names ---------------------------------------------------------
ARCHIVE = $(PLUGIN)-$(VERSION)
PACKAGE = vdr-$(ARCHIVE)
SOFILE  = libvdr-$(PLUGIN).so

### -- Default target --------------------------------------------------------
.PHONY: all i18n install install-lib install-i18n dist clean

all: $(SOFILE) i18n

### -- Compile ----------------------------------------------------------------
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $(DEFINES) $(INCLUDES) -o $@ $<

### -- Automatic header dependencies -----------------------------------------
MAKEDEP = $(CXX) -MM -MG
DEPFILE = .dependencies

$(DEPFILE): Makefile
	@$(MAKEDEP) $(CXXFLAGS) $(DEFINES) $(INCLUDES) \
	    $(OBJS:%.o=%.cpp) > $@

-include $(DEPFILE)

### -- I18N -------------------------------------------------------------------
PODIR    = po
I18Npo   = $(wildcard $(PODIR)/*.po)
I18Nmo   = $(addsuffix .mo,$(foreach f,$(I18Npo),$(basename $(f))))
I18Nmsgs = $(addprefix $(DESTDIR)$(LOCDIR)/,\
             $(addsuffix /LC_MESSAGES/vdr-$(PLUGIN).mo,\
               $(notdir $(foreach f,$(I18Npo),$(basename $(f))))))
I18Npot  = $(PODIR)/$(PLUGIN).pot

%.mo: %.po
	msgfmt -c -o $@ $<

$(I18Npot): $(wildcard *.cpp *.h)
	@mkdir -p $(PODIR)
	xgettext -C -cTRANSLATORS --no-wrap --no-location \
	    --from-code=UTF-8 \
	    -k -ktr -ktrNOOP \
	    --package-name=vdr-$(PLUGIN) \
	    --package-version=$(VERSION) \
	    --msgid-bugs-address='<see README>' \
	    -o $@ $(wildcard *.cpp *.h)

%.po: $(I18Npot)
	msgmerge -U --no-wrap --no-location --backup=none -q -N $@ $<
	@touch $@

$(I18Nmsgs): $(DESTDIR)$(LOCDIR)/%/LC_MESSAGES/vdr-$(PLUGIN).mo: $(PODIR)/%.mo
	install -D -m644 $< $@

i18n: $(I18Nmo) $(I18Npot)

install-i18n: $(I18Nmsgs)

### -- Link -------------------------------------------------------------------
$(SOFILE): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -shared \
	    $(OBJS) \
	    $(GST_LIBS) \
	    -lgobject-2.0 -lglib-2.0 \
	    -o $@

### -- Install ----------------------------------------------------------------
# VDR requires the plugin .so to carry the API version as a suffix:
#   /usr/lib/vdr/plugins/libvdr-gstreamer.so.<apiversion>
#install-lib: $(SOFILE)
#	install -D $< $(DESTDIR)$(LIBDIR)/$(SOFILE)

install-lib: $(SOFILE)
	@echo IN $(DESTDIR)$(LIBDIR)/$<
	install -D $^ $(DESTDIR)$(LIBDIR)/$^.$(APIVERSION)
	
install: install-lib install-i18n

### -- Distribution tarball ---------------------------------------------------
dist: $(I18Npo) clean
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@mkdir   $(TMPDIR)/$(ARCHIVE)
	@cp -a * $(TMPDIR)/$(ARCHIVE)
	@tar czf $(PACKAGE).tgz -C $(TMPDIR) $(ARCHIVE)
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@echo "Distribution: $(PACKAGE).tgz"

### -- Clean ------------------------------------------------------------------
clean:
	@-rm -f $(OBJS) $(DEPFILE) $(SOFILE)
	@-rm -f $(PODIR)/*.mo $(PODIR)/*.pot
	@-rm -f *.tgz core* *~
