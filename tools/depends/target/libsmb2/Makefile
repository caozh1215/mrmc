include ../../Makefile.include
DEPS= ../../Makefile.include Makefile

# lib name, version
LIBNAME=libsmb2
VERSION=1.0.0
SOURCE=$(LIBNAME)-$(VERSION)
ARCHIVE=$(SOURCE).tar.gz
REALARCHIVE=master.tar.gz
BASE_URL=https://github.com/MrMC/libsmb2/archive

CONFIGURE= cp -f $(CONFIG_SUB) $(CONFIG_GUESS) .; \
          ./configure --prefix=$(PREFIX) \
        --enable-shared \
        --disable-static \
	--without-libkrb5 \
        --disable-examples

LIBDYLIB=$(PLATFORM)/lib/.libs/libsmb2.dylib

CLEAN_FILES=$(ARCHIVE) $(PLATFORM)

all: .installed-$(PLATFORM)

$(TARBALLS_LOCATION)/$(ARCHIVE):
	$(RETRIEVE_TOOL) $(RETRIEVE_TOOL_FLAGS) $(BASE_URL)/$(REALARCHIVE)
	mv -f $(REALARCHIVE) $(TARBALLS_LOCATION)/$(ARCHIVE)

$(PLATFORM): $(TARBALLS_LOCATION)/$(ARCHIVE) $(DEPS)
	rm -rf $(PLATFORM); mkdir -p $(PLATFORM)
	cd $(PLATFORM); $(ARCHIVE_TOOL) $(ARCHIVE_TOOL_FLAGS) $(TARBALLS_LOCATION)/$(ARCHIVE)
	cd $(PLATFORM); patch -p1 < ../fixup01.patch
	cd $(PLATFORM); mkdir m4
	cd $(PLATFORM); ./bootstrap
	cd $(PLATFORM); $(CONFIGURE)

$(LIBDYLIB): $(PLATFORM)
	$(MAKE) -C $(PLATFORM)

.installed-$(PLATFORM): $(LIBDYLIB)
	#$(MAKE) -C $(PLATFORM) install
	touch $@

clean:
	$(MAKE) -C $(PLATFORM) clean
	rm -f .installed-$(PLATFORM)

distclean::
	rm -rf $(PLATFORM) .installed-$(PLATFORM)


# Replace EBADFD with EBADF Using EBADF is more portable and correct because EBADFD means a file descriptor in bad state, not an invalid file descriptor.