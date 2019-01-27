DESTDIR      ?=
PREFIX        = $(DESTDIR)/usr/local
INSTALL       = /usr/bin/install -D
MSGFMT        = /usr/bin/msgfmt
SED           = /bin/sed
bindir        = $(PREFIX)/bin
libdir        = $(PREFIX)/lib
sysconfdir    = $(PREFIX)/etc
datarootdir   = ${PREFIX}/share
datadir       = ${datarootdir}

CXXFLAGS += -std=gnu++11 -g3

all: libprimus_vk.so libnv_vulkan_wrapper.so

libprimus_vk.so: primus_vk.cpp
	g++ $(CXXFLAGS) -I/usr/include/vulkan -shared -fPIC $^ -o $@

libnv_vulkan_wrapper.so: nv_vulkan_wrapper.cpp
	g++ $(CXXFLAGS) -I/usr/include/vulkan -shared -fPIC $^ -o $@

primus_vk_diag: primus_vk_diag.o
	g++ -g3 -o $@ $^ -lX11 -lvulkan -ldl

install:
	$(INSTALL) "libnv_vulkan_wrapper.so" \
		"$(libdir)/libnv_vulkan_wrapper.so"
	$(INSTALL) "libprimus_vk.so" "$(libdir)/libprimus_vk.so"
	$(INSTALL) -m644 "primus_vk.json" \
		"$(datadir)/vulkan/implicit_layer.d/primus_vk.json"
	$(INSTALL) -Dm644 "nv_vulkan_wrapper.json" \
		"$(datadir)/vulkan/icd.d/nv_vulkan_wrapper.json"
	$(INSTALL) -m755 "pvkrun.in.sh" "$(bindir)/pvkrun"
