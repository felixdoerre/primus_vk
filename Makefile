DESTDIR      ?=
PREFIX        = $(DESTDIR)/usr/local
INSTALL       = /usr/bin/install
override INSTALL += -D
MSGFMT        = /usr/bin/msgfmt
SED           = /bin/sed
bindir        = $(PREFIX)/bin
libdir        = $(PREFIX)/lib
sysconfdir    = $(PREFIX)/etc
datarootdir   = ${PREFIX}/share
datadir       = ${datarootdir}

override CXXFLAGS += --std=gnu++11 -g3

all: libprimus_vk.so libnv_vulkan_wrapper.so

libprimus_vk.so: primus_vk.cpp
	g++ $(CXXFLAGS) -I/usr/include/vulkan -shared -fPIC $^ -o $@ -Wl,-soname,libprimus_vk.so.1 -ldl -lpthread $(LDFLAGS)

libnv_vulkan_wrapper.so: nv_vulkan_wrapper.cpp
	g++ $(CXXFLAGS) -I/usr/include/vulkan -shared -fPIC $^ -o $@ -Wl,-soname,libnv_vulkan_wrapper.so.1 -ldl $(LDFLAGS)

primus_vk_forwarding.h:
	xsltproc surface_forwarding_functions.xslt /usr/share/vulkan/registry/vk.xml | tail -n +2 > $@

primus_vk_forwarding_prototypes.h:
	xsltproc surface_forwarding_prototypes.xslt /usr/share/vulkan/registry/vk.xml | tail -n +2 > $@

primus_vk.cpp: primus_vk_forwarding.h primus_vk_forwarding_prototypes.h

primus_vk_diag: primus_vk_diag.o
	g++ -g3 -o $@ $^ -lX11 -lvulkan -ldl $(LDFLAGS)

clean:
	rm -f libnv_vulkan_wrapper.so libprimus_vk.so

install: all
	$(INSTALL) "libnv_vulkan_wrapper.so" -t "$(libdir)/"
	$(INSTALL) "libprimus_vk.so"  -t "$(libdir)/"
	$(INSTALL) -m644 "primus_vk.json" -t "$(datadir)/vulkan/implicit_layer.d/"
	$(INSTALL) -m644 "nv_vulkan_wrapper.json" -t "$(datadir)/vulkan/icd.d/"
	$(INSTALL) -m755 "pvkrun.in.sh" "$(bindir)/pvkrun"
