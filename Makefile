.DEFAULT_GOAL := debug
CMAKE ?= cmake
CTEST ?= ctest

.PHONY: debug release test sanitizer thread-sanitizer fmt check-format doc tags clean

debug:
	$(CMAKE) --preset debug
	$(CMAKE) --build --preset debug

release:
	$(CMAKE) --preset release
	$(CMAKE) --build --preset release
	$(CTEST) --preset release

test: debug
	$(CTEST) --preset debug

sanitizer:
	$(CMAKE) --preset sanitizer
	$(CMAKE) --build --preset sanitizer
	$(CTEST) --preset sanitizer

thread-sanitizer:
	$(CMAKE) --preset thread-sanitizer
	$(CMAKE) --build --preset thread-sanitizer
	$(CTEST) --preset thread-sanitizer

fmt:
	$(CMAKE) -DTB_FORMAT_MODE=write -P cmake/Format.cmake

check-format:
	$(CMAKE) -DTB_FORMAT_MODE=check -P cmake/Format.cmake

doc: debug
	@command -v doxygen >/dev/null || { echo "Doxygen is optional and must already be installed."; exit 1; }
	@(cat Doxyfile; echo "PROJECT_NUMBER = $$(cat VERSION)") | doxygen -

tags:
	ctags

clean:
	$(CMAKE) -E rm -rf build/debug build/release build/sanitizer build/thread-sanitizer
