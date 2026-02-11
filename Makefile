.PHONY: all dist clean distclean print-name help build rpm srpm deb arch nix

# Extract version from CMakeLists.txt (preferred method)
VERSION := $(shell grep -E 'VERSION\s+[0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')

# If VERSION is empty, try git tag
ifeq ($(VERSION),)
  VERSION := $(shell git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')
endif

# If VERSION is still empty, use 0.1.0 as fallback
ifeq ($(VERSION),)
  VERSION := 0.1.0
endif

# Get git hash for archive naming
GITHASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

# Release timestamp: 0.YYYYMMDD_HHMM
RELEASE_TS := 0.$(shell date +%Y%m%d_%H%M)

# Archive name: ucc-<version>-<githash>.tar.gz
DISTNAME := ucc-$(VERSION)-$(GITHASH)
ARCHIVE := $(DISTNAME).tar.gz

# Temporary directory used when creating the dist tarball
TMPDIR := dist/$(DISTNAME)-tmp

# Exclude patterns for tar (paths are relative to repo root)
EXCLUDES := --exclude=build --exclude=dist --exclude=.git --exclude='$(TMPDIR)' --exclude='*.o' --exclude='*.a' --exclude='*.so*'

help:
	@echo "Uniwill Control Center - Build and Package Targets"
	@echo ""
	@echo "Build targets:"
	@echo "  make build              Build the project locally"
	@echo "  make clean              Clean build artifacts"
	@echo ""
	@echo "Package targets:"
	@echo "  make dist               Create source tarball"
	@echo "  make rpm                Create RPM packages (Fedora/RHEL)"
	@echo "  make srpm               Create SRPM only (Fedora/RHEL)"
	@echo "  make deb                Create Debian packages (Debian/Ubuntu)"
	@echo "  make arch               Create Arch Linux package"
	@echo "  make nix                Create Nix package"
	@echo ""
	@echo "Other:"
	@echo "  make help               Show this help message"
	@echo "  make print-name         Print archive name"

all: build

build:
	@echo "Building UCC..."
	@./build-local.sh

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf build dist
	@find . -name "CMakeCache.txt" -delete
	@find . -name "CMakeFiles" -type d -exec rm -rf {} + 2>/dev/null || true
	@echo "Clean complete"

distclean: clean
	@echo "Removing distribution artifacts..."
	@rm -f $(ARCHIVE) ucc-*.tar.gz
	@rm -rf dist
	@echo "Distclean complete"

dist: distclean
	@echo "Creating source archive: $(ARCHIVE)"
	@mkdir -p dist
	@rm -rf $(TMPDIR); mkdir -p $(TMPDIR)
	@echo "Preparing source tree in $(TMPDIR)/ucc-$(VERSION)..."
	@rsync -a $(EXCLUDES) ./ $(TMPDIR)/ucc-$(VERSION)
	@tar czf dist/$(ARCHIVE) -C $(TMPDIR) ucc-$(VERSION)
	@rm -rf $(TMPDIR)
	@echo "Created dist/$(ARCHIVE)"
	@cd dist && sha256sum $(ARCHIVE) > $(ARCHIVE).sha256
	@echo "SHA256: $$(cat dist/$(ARCHIVE).sha256)"

print-name:
	@echo $(DISTNAME)

rpm: dist
	@echo "Building RPM packages..."
	@mkdir -p dist/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
	@cp dist/$(ARCHIVE) dist/rpmbuild/SOURCES/
	@# Create modified spec file with correct version, release, and source filename
	@sed -e 's/^Version:.*/Version:        $(VERSION)/' \
	     -e 's/^Release:.*/Release:        $(RELEASE_TS)/' \
	     -e 's/Source0:.*/Source0:        $(ARCHIVE)/' \
	     ucc.spec > dist/rpmbuild/SPECS/ucc.spec
	@rpmbuild -ba \
		--define "_topdir $$(pwd)/dist/rpmbuild" \
		--define "_builddir %{_topdir}/BUILD" \
		--define "_rpmdir %{_topdir}/RPMS" \
		--define "_srcrpmdir %{_topdir}/SRPMS" \
		dist/rpmbuild/SPECS/ucc.spec
	@echo "RPM packages created in dist/rpmbuild/RPMS/"
	@echo "SRPM created in dist/rpmbuild/SRPMS/"

srpm: dist
	@echo "Building SRPM package..."
	@mkdir -p dist/rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
	@cp dist/$(ARCHIVE) dist/rpmbuild/SOURCES/
	@# Create modified spec file with correct version, release, and source filename
	@sed -e 's/^Version:.*/Version:        $(VERSION)/' \
	     -e 's/^Release:.*/Release:        $(RELEASE_TS)/' \
	     -e 's/Source0:.*/Source0:        $(ARCHIVE)/' \
	     ucc.spec > dist/rpmbuild/SPECS/ucc.spec
	@rpmbuild -bs \
		--define "_topdir $$(pwd)/dist/rpmbuild" \
		--define "_builddir %{_topdir}/BUILD" \
		--define "_rpmdir %{_topdir}/RPMS" \
		--define "_srcrpmdir %{_topdir}/SRPMS" \
		dist/rpmbuild/SPECS/ucc.spec
	@echo "SRPM created in dist/rpmbuild/SRPMS/"

deb: dist
	@echo "Building Debian packages..."
	@mkdir -p dist/deb
	@cd dist && tar xzf $(ARCHIVE)
	@cp -r debian dist/ucc-$(VERSION)/
	@cd dist/ucc-$(VERSION) && debuild -uc -us
	@echo "Debian packages created in dist/"

arch: dist
	@echo "Building Arch Linux package..."
	@mkdir -p dist/arch
	@cp PKGBUILD dist/arch/
	@cd dist/arch && \
		sed -i "s/pkgver=.*/pkgver=$(VERSION)/" PKGBUILD && \
		sed -i "s/pkgrel=.*/pkgrel=1/" PKGBUILD && \
		cp ../$(ARCHIVE) . && \
		BUILDDIR=$$(pwd)/build SRCDEST=$$(pwd) makepkg -sf --noconfirm
	@echo "Arch package created in dist/arch/"

nix: dist
	@echo "Building Nix package..."
	@mkdir -p dist/nix
	@cp default.nix dist/nix/
	@cd dist/nix && \
		SHA=$$(sha256sum ../$(ARCHIVE) | cut -d' ' -f1) && \
		sed -i "s/hash = .*/hash = \"sha256-$${SHA}=\";/" default.nix && \
		nix-build default.nix
	@echo "Nix package built (check result symlink)"


