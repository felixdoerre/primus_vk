DESTDIR      ?=
PREFIX        = /usr/local
INSTALL       = /usr/bin/install
override INSTALL += -D
MSGFMT        = /usr/bin/msgfmt
SED           = /bin/sed
LN            = /bin/ln
bindir        = $(PREFIX)/bin
libdir        = $(PREFIX)/lib
sysconfdir    = $(PREFIX)/etc
datarootdir   = ${PREFIX}/share
datadir       = ${datarootdir}

override CXXFLAGS += --std=c++17 -g3 -I/usr/include/vulkan

all: libprimus_vk.so libnv_vulkan_wrapper.so

libprimus_vk.so: primus_vk.cpp  primus_vk_forwarding.h primus_vk_forwarding_prototypes.h primus_vk_dispatch_table.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared -fPIC primus_vk.cpp -o $@ -Wl,-soname,libprimus_vk.so.1 -ldl -lpthread $(LDFLAGS)

libnv_vulkan_wrapper.so: nv_vulkan_wrapper.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared -fPIC $^ -o $@ -Wl,-soname,libnv_vulkan_wrapper.so.1 -lX11 -lGLX -ldl $(LDFLAGS)

primus_vk_forwarding.h:
	xsltproc surface_forwarding_functions.xslt /usr/share/vulkan/registry/vk.xml | tail -n +2 > $@

primus_vk_forwarding_prototypes.h:
	xsltproc surface_forwarding_prototypes.xslt /usr/share/vulkan/registry/vk.xml | tail -n +2 > $@

primus_vk_diag: primus_vk_diag.o
	$(CXX) -g3 -o $@ $^ -lX11 -lvulkan -ldl -lpthread $(LDFLAGS)

clean:
	rm -f libnv_vulkan_wrapper.so libprimus_vk.so

install: all
	$(INSTALL) "libnv_vulkan_wrapper.so" "$(DESTDIR)$(libdir)/libnv_vulkan_wrapper.so.1"
	$(INSTALL) "libprimus_vk.so"  "$(DESTDIR)$(libdir)/libprimus_vk.so.1"
	$(INSTALL) -m644 "primus_vk.json" -t "$(DESTDIR)$(datadir)/vulkan/implicit_layer.d/"
	$(INSTALL) -m644 "nv_vulkan_wrapper.json" -t "$(DESTDIR)$(datadir)/vulkan/icd.d/"
	$(INSTALL) -m755 "pvkrun.in.sh" "$(DESTDIR)$(bindir)/pvkrun"
