-include config.mk

BUILDTYPE ?= Release
PYTHON ?= python
DESTDIR ?=
SIGN ?=
PREFIX ?= /usr/local

# Determine EXEEXT
EXEEXT := $(shell $(PYTHON) -c \
		"import sys; print('.exe' if sys.platform == 'win32' else '')")

NODE ?= ./iojs$(EXEEXT)
NODE_EXE = iojs$(EXEEXT)
NODE_G_EXE = iojs_g$(EXEEXT)

# Default to verbose builds.
# To do quiet/pretty builds, run `make V=` to set V to an empty string,
# or set the V environment variable to an empty string.
V ?= 1

# BUILDTYPE=Debug builds both release and debug builds. If you want to compile
# just the debug build, run `make -C out BUILDTYPE=Debug` instead.
ifeq ($(BUILDTYPE),Release)
all: out/Makefile $(NODE_EXE)
else
all: out/Makefile $(NODE_EXE) $(NODE_G_EXE)
endif

# The .PHONY is needed to ensure that we recursively use the out/Makefile
# to check for changes.
.PHONY: $(NODE_EXE) $(NODE_G_EXE)

$(NODE_EXE): config.gypi out/Makefile
	$(MAKE) -C out BUILDTYPE=Release V=$(V)
	ln -fs out/Release/$(NODE_EXE) $@

$(NODE_G_EXE): config.gypi out/Makefile
	$(MAKE) -C out BUILDTYPE=Debug V=$(V)
	ln -fs out/Debug/$(NODE_EXE) $@

out/Makefile: common.gypi deps/uv/uv.gyp deps/http_parser/http_parser.gyp deps/zlib/zlib.gyp deps/v8/build/toolchain.gypi deps/v8/build/features.gypi deps/v8/tools/gyp/v8.gyp node.gyp config.gypi
	$(PYTHON) tools/gyp_node.py -f make

config.gypi: configure
	if [ -f $@ ]; then
		$(error Stale $@, please re-run ./configure)
	else
		$(error No $@, please run ./configure first)
	fi

install: all
	$(PYTHON) tools/install.py $@ '$(DESTDIR)' '$(PREFIX)'

uninstall:
	$(PYTHON) tools/install.py $@ '$(DESTDIR)' '$(PREFIX)'

clean:
	-rm -rf out/Makefile $(NODE_EXE) $(NODE_G_EXE) out/$(BUILDTYPE)/$(NODE_EXE) blog.html email.md
	@if [ -d out ]; then find out/ -name '*.o' -o -name '*.a' | xargs rm -rf; fi
	-rm -rf node_modules

distclean:
	-rm -rf out
	-rm -f config.gypi icu_config.gypi
	-rm -f config.mk
	-rm -rf $(NODE_EXE) $(NODE_G_EXE) blog.html email.md
	-rm -rf node_modules
	-rm -rf deps/icu
	-rm -rf deps/icu4c*.tgz deps/icu4c*.zip deps/icu-tmp
	-rm -f $(BINARYTAR).* $(TARBALL).*

check: test

cctest: all
	@out/$(BUILDTYPE)/$@

test: | cctest  # Depends on 'all'.
	$(PYTHON) tools/test.py --mode=release message parallel sequential -J
	$(MAKE) jslint
	$(MAKE) cpplint

test-parallel: all
	$(PYTHON) tools/test.py --mode=release parallel -J

test-valgrind: all
	$(PYTHON) tools/test.py --mode=release --valgrind sequential parallel message

test/gc/node_modules/weak/build/Release/weakref.node: $(NODE_EXE)
	$(NODE) deps/npm/node_modules/node-gyp/bin/node-gyp rebuild \
		--directory="$(shell pwd)/test/gc/node_modules/weak" \
		--nodedir="$(shell pwd)"

build-addons: $(NODE_EXE)
	rm -rf test/addons/doc-*/
	$(NODE) tools/doc/addon-verify.js
	$(foreach dir, \
			$(sort $(dir $(wildcard test/addons/*/*.gyp))), \
			$(NODE) deps/npm/node_modules/node-gyp/bin/node-gyp rebuild \
					--directory="$(shell pwd)/$(dir)" \
					--nodedir="$(shell pwd)" && ) echo "build done"

test-gc: all test/gc/node_modules/weak/build/Release/weakref.node
	$(PYTHON) tools/test.py --mode=release gc

test-build: all build-addons

test-all: test-build test/gc/node_modules/weak/build/Release/weakref.node
	$(PYTHON) tools/test.py --mode=debug,release

test-all-valgrind: test-build
	$(PYTHON) tools/test.py --mode=debug,release --valgrind

test-ci:
	$(PYTHON) tools/test.py -p tap --logfile test.tap --mode=release message parallel sequential

test-release: test-build
	$(PYTHON) tools/test.py --mode=release

test-debug: test-build
	$(PYTHON) tools/test.py --mode=debug

test-message: test-build
	$(PYTHON) tools/test.py message

test-simple: | cctest  # Depends on 'all'.
	$(PYTHON) tools/test.py parallel sequential

test-pummel: all
	$(PYTHON) tools/test.py pummel

test-internet: all
	$(PYTHON) tools/test.py internet

test-debugger: all
	$(PYTHON) tools/test.py debugger

test-npm: $(NODE_EXE)
	NODE_EXE=$(NODE_EXE) tools/test-npm.sh

test-npm-publish: $(NODE_EXE)
	npm_package_config_publishtest=true $(NODE) deps/npm/test/run.js

test-addons: test-build
	$(PYTHON) tools/test.py --mode=release addons

test-timers:
	$(MAKE) --directory=tools faketime
	$(PYTHON) tools/test.py --mode=release timers

test-timers-clean:
	$(MAKE) --directory=tools clean

apidoc_sources = $(wildcard doc/api/*.markdown)
apidocs = $(addprefix out/,$(apidoc_sources:.markdown=.html)) \
          $(addprefix out/,$(apidoc_sources:.markdown=.json))

apidoc_dirs = out/doc out/doc/api/ out/doc/api/assets

apiassets = $(subst api_assets,api/assets,$(addprefix out/,$(wildcard doc/api_assets/*)))

doc: $(apidoc_dirs) $(apiassets) $(apidocs) tools/doc/ $(NODE_EXE)

$(apidoc_dirs):
	mkdir -p $@

out/doc/api/assets/%: doc/api_assets/% out/doc/api/assets/
	cp $< $@

out/doc/%: doc/%
	cp -r $< $@

out/doc/api/%.json: doc/api/%.markdown $(NODE_EXE)
	$(NODE) tools/doc/generate.js --format=json $< > $@

out/doc/api/%.html: doc/api/%.markdown $(NODE_EXE)
	$(NODE) tools/doc/generate.js --format=html --template=doc/template.html $< > $@

docopen: out/doc/api/all.html
	-google-chrome out/doc/api/all.html

docclean:
	-rm -rf out/doc

RAWVER=$(shell $(PYTHON) tools/getnodeversion.py)
VERSION=v$(RAWVER)
FULLVERSION=$(VERSION)
RELEASE=$(shell sed -ne 's/\#define NODE_VERSION_IS_RELEASE \([01]\)/\1/p' src/node_version.h)
PLATFORM=$(shell uname | tr '[:upper:]' '[:lower:]')
NPMVERSION=v$(shell cat deps/npm/package.json | grep '"version"' | sed 's/^[^:]*: "\([^"]*\)",.*/\1/')
ifeq ($(findstring x86_64,$(shell uname -m)),x86_64)
DESTCPU ?= x64
else
DESTCPU ?= ia32
endif
ifeq ($(DESTCPU),x64)
ARCH=x64
else
ifeq ($(DESTCPU),arm)
ARCH=arm
else
ARCH=x86
endif
endif
ifdef NIGHTLY
TAG = nightly-$(NIGHTLY)
FULLVERSION=$(VERSION)-$(TAG)
endif
TARNAME=iojs-$(FULLVERSION)
TARBALL=$(TARNAME).tar
BINARYNAME=$(TARNAME)-$(PLATFORM)-$(ARCH)
BINARYTAR=$(BINARYNAME).tar
XZ=$(shell which xz > /dev/null 2>&1; echo $$?)
XZ_COMPRESSION ?= 9
PKG=out/$(TARNAME).pkg
PACKAGEMAKER ?= /Developer/Applications/Utilities/PackageMaker.app/Contents/MacOS/PackageMaker

PKGSRC=iojs-$(DESTCPU)-$(RAWVER).tgz
ifdef NIGHTLY
PKGSRC=iojs-$(DESTCPU)-$(RAWVER)-$(TAG).tgz
endif

dist: doc $(TARBALL) $(PKG)

PKGDIR=out/dist-osx

release-only:
	@if [ "$(shell git status --porcelain | egrep -v '^\?\? ')" = "" ]; then \
		exit 0 ; \
	else \
	  echo "" >&2 ; \
		echo "The git repository is not clean." >&2 ; \
		echo "Please commit changes before building release tarball." >&2 ; \
		echo "" >&2 ; \
		git status --porcelain | egrep -v '^\?\?' >&2 ; \
		echo "" >&2 ; \
		exit 1 ; \
	fi
	@if [ "$(NIGHTLY)" != "" -o "$(RELEASE)" = "1" ]; then \
		exit 0; \
	else \
	  echo "" >&2 ; \
		echo "#NODE_VERSION_IS_RELEASE is set to $(RELEASE)." >&2 ; \
	  echo "Did you remember to update src/node_version.h?" >&2 ; \
	  echo "" >&2 ; \
		exit 1 ; \
	fi

pkg: $(PKG)

$(PKG): release-only
	rm -rf $(PKGDIR)
	rm -rf out/deps out/Release
	$(PYTHON) ./configure --dest-cpu=ia32 --tag=$(TAG)
	$(MAKE) install V=$(V) DESTDIR=$(PKGDIR)/32
	rm -rf out/deps out/Release
	$(PYTHON) ./configure --dest-cpu=x64 --tag=$(TAG)
	$(MAKE) install V=$(V) DESTDIR=$(PKGDIR)
	SIGN="$(APP_SIGN)" PKGDIR="$(PKGDIR)" bash tools/osx-codesign.sh
	lipo $(PKGDIR)/32/usr/local/bin/iojs \
		$(PKGDIR)/usr/local/bin/iojs \
		-output $(PKGDIR)/usr/local/bin/iojs-universal \
		-create
	mv $(PKGDIR)/usr/local/bin/iojs-universal $(PKGDIR)/usr/local/bin/iojs
	rm -rf $(PKGDIR)/32
	cat tools/osx-pkg.pmdoc/index.xml.tmpl | sed -e 's|__iojsversion__|'$(FULLVERSION)'|g' | sed -e 's|__npmversion__|'$(NPMVERSION)'|g' > tools/osx-pkg.pmdoc/index.xml
	$(PACKAGEMAKER) \
		--id "org.nodejs.Node" \
		--doc tools/osx-pkg.pmdoc \
		--out $(PKG)
	SIGN="$(INT_SIGN)" PKG="$(PKG)" bash tools/osx-productsign.sh

$(TARBALL): release-only $(NODE_EXE) doc
	git checkout-index -a -f --prefix=$(TARNAME)/
	mkdir -p $(TARNAME)/doc/api
	cp doc/iojs.1 $(TARNAME)/doc/iojs.1
	cp -r out/doc/api/* $(TARNAME)/doc/api/
	rm -rf $(TARNAME)/deps/v8/{test,samples,tools/profviz} # too big
	rm -rf $(TARNAME)/doc/images # too big
	rm -rf $(TARNAME)/deps/uv/{docs,samples,test}
	rm -rf $(TARNAME)/deps/openssl/{doc,demos,test}
	rm -rf $(TARNAME)/deps/zlib/contrib # too big, unused
	find $(TARNAME)/ -type l | xargs rm # annoying on windows
	tar -cf $(TARNAME).tar $(TARNAME)
	rm -rf $(TARNAME)
	gzip -c -f -9 $(TARNAME).tar > $(TARNAME).tar.gz
ifeq ($(XZ), 0)
	xz -c -f -$(XZ_COMPRESSION) $(TARNAME).tar > $(TARNAME).tar.xz
endif
	rm $(TARNAME).tar

tar: $(TARBALL)

$(BINARYTAR): release-only
	rm -rf $(BINARYNAME)
	rm -rf out/deps out/Release
	$(PYTHON) ./configure --prefix=/ --dest-cpu=$(DESTCPU) --tag=$(TAG) $(CONFIG_FLAGS)
	$(MAKE) install DESTDIR=$(BINARYNAME) V=$(V) PORTABLE=1
	cp README.md $(BINARYNAME)
	cp LICENSE $(BINARYNAME)
	cp CHANGELOG.md $(BINARYNAME)
	tar -cf $(BINARYNAME).tar $(BINARYNAME)
	rm -rf $(BINARYNAME)
	gzip -c -f -9 $(BINARYNAME).tar > $(BINARYNAME).tar.gz
ifeq ($(XZ), 0)
	xz -c -f -$(XZ_COMPRESSION) $(BINARYNAME).tar > $(BINARYNAME).tar.xz
endif
	rm $(BINARYNAME).tar

binary: $(BINARYTAR)

$(PKGSRC): release-only
	rm -rf dist out
	$(PYTHON) configure --prefix=/ \
		--dest-cpu=$(DESTCPU) --tag=$(TAG) $(CONFIG_FLAGS)
	$(MAKE) install DESTDIR=dist
	(cd dist; find * -type f | sort) > packlist
	pkg_info -X pkg_install | \
		egrep '^(MACHINE_ARCH|OPSYS|OS_VERSION|PKGTOOLS_VERSION)' > build-info
	pkg_create -B build-info -c tools/pkgsrc/comment -d tools/pkgsrc/description \
		-f packlist -I /opt/local -p dist -U $(PKGSRC)

pkgsrc: $(PKGSRC)

haswrk=$(shell which wrk > /dev/null 2>&1; echo $$?)
wrk:
ifneq ($(haswrk), 0)
	@echo "please install wrk before proceeding. More information can be found in benchmark/README.md." >&2
	@exit 1
endif

bench-net: all
	@$(NODE) benchmark/common.js net

bench-crypto: all
	@$(NODE) benchmark/common.js crypto

bench-tls: all
	@$(NODE) benchmark/common.js tls

bench-http: wrk all
	@$(NODE) benchmark/common.js http

bench-fs: all
	@$(NODE) benchmark/common.js fs

bench-misc: all
	@$(MAKE) -C benchmark/misc/function_call/
	@$(NODE) benchmark/common.js misc

bench-array: all
	@$(NODE) benchmark/common.js arrays

bench-buffer: all
	@$(NODE) benchmark/common.js buffers

bench-url: all
	@$(NODE) benchmark/common.js url

bench-events: all
	@$(NODE) benchmark/common.js events

bench-all: bench bench-misc bench-array bench-buffer bench-url bench-events

bench: bench-net bench-http bench-fs bench-tls

bench-http-simple:
	 benchmark/http_simple_bench.sh

bench-idle:
	$(NODE) benchmark/idle_server.js &
	sleep 1
	$(NODE) benchmark/idle_clients.js &

jslint:
	$(NODE) tools/eslint/bin/eslint.js src lib test --rulesdir tools/eslint-rules --reset --quiet

CPPLINT_EXCLUDE ?=
CPPLINT_EXCLUDE += src/node_lttng.cc
CPPLINT_EXCLUDE += src/node_root_certs.h
CPPLINT_EXCLUDE += src/node_lttng_tp.h
CPPLINT_EXCLUDE += src/node_win32_perfctr_provider.cc
CPPLINT_EXCLUDE += src/queue.h
CPPLINT_EXCLUDE += src/tree.h
CPPLINT_EXCLUDE += src/v8abbr.h

CPPLINT_FILES = $(filter-out $(CPPLINT_EXCLUDE), $(wildcard src/*.cc src/*.h src/*.c tools/icu/*.h tools/icu/*.cc deps/debugger-agent/include/* deps/debugger-agent/src/*))

cpplint:
	@$(PYTHON) tools/cpplint.py $(CPPLINT_FILES)

lint: jslint cpplint

.PHONY: lint cpplint jslint bench clean docopen docclean doc dist distclean \
	check uninstall install install-includes install-bin all staticlib \
	dynamiclib test test-all test-addons build-addons website-upload pkg \
	blog blogclean tar binary release-only bench-http-simple bench-idle \
	bench-all bench bench-misc bench-array bench-buffer bench-net \
	bench-http bench-fs bench-tls cctest
