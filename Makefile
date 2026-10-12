.PHONY: all clean
all:
	$(MAKE) -f Makefile.wii
clean:
	$(MAKE) -f Makefile.wii clean

.PHONY: check package
check:
	tools/check-core.sh
package: all
	python3 tools/package.py
