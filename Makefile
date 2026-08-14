#Thanks to https://stackoverflow.com/questions/52034997/how-to-make-makefile-recompile-when-a-header-file-is-changed for the -MMD & -MP flags
#Without them headers wouldn't trigger recompilation

#Force g++ cause clang crashes on some hooks
CXX := g++
STEAMCLIENT ?= $(HOME)/.local/share/Steam/ubuntu12_32/steamclient.so
TSUKI_ROOT ?=

libs := $(wildcard lib/*.a)
srcs := $(shell find src/ -type f -iname "*.cpp")
objs := $(srcs:src/%.cpp=obj/%.o)
deps := $(objs:%.o=%.d)

CXXFLAGS := -O3 -flto=auto -fPIC -m32 -std=c++20 -fno-reorder-blocks-and-partition -Wall -Wextra -Wpedantic -Wno-error=format-security -D_GLIBCXX_USE_CXX11_ABI=0
CXXFLAGS += -floop-block -fgraphite-identity -floop-parallelize-all -pipe -fopenmp -fomit-frame-pointer

LDFLAGS := -shared -Wl,--no-undefined
LDFLAGS += $(shell pkg-config --libs "openssl")
LDFLAGS += $(shell pkg-config --libs "libcurl")

JOBS := $(shell nproc)

#DATE := $(shell date "+%Y%m%d%H%M%S")
DATE := $(shell cat res/version.txt)

ifeq ($(shell echo $$NATIVE),1)
	CXXFLAGS += -march=native
endif

#Speed up compilation if additional dependencies are found
ifeq ($(shell type ccache &> /dev/null && echo "found"),found)
	export PATH := /usr/lib/ccache/bin:$(PATH)
endif
ifeq ($(shell type mold &> /dev/null && echo "found"),found)
	LDFLAGS += -fuse-ld=mold
endif

audit-libs:
	$(MAKE) -j $(JOBS) bin/SLSsteam.so bin/library-inject.so bin/sls-prelaunch bin/slssteam-control

ronin-module: audit-libs
	cp bin/SLSsteam.so module/payload/SLSsteam.so
	cp bin/library-inject.so module/payload/library-inject.so
	cp bin/sls-prelaunch module/payload/sls-prelaunch
	cp bin/slssteam-control module/payload/slssteam-control
	mkdir -p module/assets/steamdb-history-extension
	cp tools/steamdb-history-extension/* module/assets/steamdb-history-extension/

deploy-tsuki-module: ronin-module
	@test -n "$(TSUKI_ROOT)" || { echo "usage: make deploy-tsuki-module TSUKI_ROOT=/path/to/tsuki"; exit 2; }
	sh scripts/deploy-tsuki-module.sh "$(TSUKI_ROOT)"

rollback-tsuki-module:
	@test -n "$(TSUKI_ROOT)" || { echo "usage: make rollback-tsuki-module TSUKI_ROOT=/path/to/tsuki"; exit 2; }
	sh scripts/deploy-tsuki-module.sh --rollback "$(TSUKI_ROOT)"

test-manifestpin-patterns:
	g++ -std=c++20 tools/test_manifestpin_patterns.cpp -o /tmp/test_manifestpin_patterns
	/tmp/test_manifestpin_patterns "$(STEAMCLIENT)"

test-reconcilepin-pattern:
	g++ -std=c++20 tools/test_reconcilepin_pattern.cpp -o /tmp/test_reconcilepin_pattern
	/tmp/test_reconcilepin_pattern "$(STEAMCLIENT)"

test-depotquarantine-patterns:
	g++ -std=c++20 tools/test_depotquarantine_patterns.cpp -o /tmp/test_depotquarantine_patterns
	/tmp/test_depotquarantine_patterns "$(STEAMCLIENT)"

test-steamstub:
	g++ -O2 -std=c++20 -Wall -Wextra -Wpedantic tools/test_steamstub_ticket.cpp -o /tmp/test_steamstub_ticket
	/tmp/test_steamstub_ticket

test-firstseen:
	g++ -O2 -std=c++20 -Wall -Wextra -Wpedantic tools/test_firstseen.cpp -o /tmp/test_firstseen
	/tmp/test_firstseen

test-slssteam-control: bin/slssteam-control
	UV_CACHE_DIR=$${UV_CACHE_DIR:-/tmp/codex-uv-cache} SLSSTEAM_CONTROL_BIN=bin/slssteam-control uv run python tools/test_slssteam_control.py

tools:
	$(MAKE) -j 2 schema-grabber ticket-grabber

bin/SLSsteam.so: $(objs) $(libs)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $^ -o bin/SLSsteam.so $(LDFLAGS)

bin/library-inject.so:
	@mkdir -p bin
	$(MAKE) -C tools/library-inject
	ln tools/library-inject/library-inject.so bin/library-inject.so

bin/sls-prelaunch: tools/sls-prelaunch.cpp $(filter-out obj/main.o,$(objs)) $(libs)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -Iinclude $^ -o $@ $(filter-out -shared,$(LDFLAGS))

bin/slssteam-control: tools/slssteam-control.cpp
	@mkdir -p bin
	g++ -O2 -std=c++20 -Wall -Wextra -Wpedantic $< -o $@ $(shell pkg-config --libs "openssl")

schema-grabber:
	$(MAKE) -C tools/schema-grabber

ticket-grabber:
	$(MAKE) -C tools/ticket-grabber

-include $(deps)
obj/update.o: src/update.cpp res/version.txt
	$(shell ./embed-version.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

-include $(deps)
obj/config.o: src/config.cpp res/config.yaml
	$(shell ./embed-config.sh)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

# module/module.json's "id" is these files' only source for the Ronin
# runtime-binding suffix. One atomic generated-header target avoids two
# parallel object recipes racing to overwrite the same source file.
src/ronin_env.hpp: module/module.json embed-module-env.sh
	./embed-module-env.sh

obj/log.o obj/main.o: src/ronin_env.hpp

-include $(deps)
obj/%.o : src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -isysteminclude -MMD -MP -c $< -o $@

clean-libs:
	rm -rvf \
		"obj/" \
		"bin/" \
		"zips/" \
		"tools/ticket-grabber/bin" \
		"tools/ticket-grabber/obj" \
		"tools/schema-grabber/bin" \
		"tools/schema-grabber/obj"

clean-tools:
	$(MAKE) -C tools/schema-grabber clean
	$(MAKE) -C tools/ticket-grabber clean

install:
	sh setup.sh install

uninstall:
	sh setup.sh uninstall

zips: build
	@mkdir -p zips
	7z a -mx9 -m9=lzma2 \
		"zips/SLSsteam $(DATE).7z" \
		"bin/SLSsteam.so" \
		"bin/library-inject.so" \
		"setup.sh" \
		"docs/LICENSE" \
		"res/config.yaml" \
		"tools/ticket-grabber/bin/Release/net9.0/linux-x64/publish/ticket-grabber" \
		"tools/schema-grabber/bin/Release/net9.0/linux-x64/publish/schema-grabber"

	#Compatibility for Github issues
	7z a -mx9 -m9=lzma \
		"zips/SLSsteam $(DATE).zip" \
		"bin/SLSsteam.so" \
		"bin/library-inject.so" \
		"setup.sh" \
		"docs/LICENSE" \
		"res/config.yaml" \
		"tools/ticket-grabber/bin/Release/net9.0/linux-x64/publish/ticket-grabber" \
		"tools/schema-grabber/bin/Release/net9.0/linux-x64/publish/schema-grabber"

zips-config:
	7z a -mx9 -m9=lzma "zips/SLSsteam - SLSConfig $(DATE).zip" "$(HOME)/.config/SLSsteam/config.yaml"
	#Compatibility for Github issues
	7z a -mx9 -m9=lzma2 "zips/SLSsteam - SLSConfig $(DATE).7z" "$(HOME)/.config/SLSsteam/config.yaml"


clean: clean-libs clean-tools
build: audit-libs tools
rebuild: clean build
release: rebuild zips

.PHONY: audit-libs ronin-module deploy-tsuki-module rollback-tsuki-module \
	test-manifestpin-patterns test-reconcilepin-pattern \
	test-depotquarantine-patterns test-steamstub \
	test-firstseen test-slssteam-control build clean clean-libs clean-tools \
	tools rebuild zips
