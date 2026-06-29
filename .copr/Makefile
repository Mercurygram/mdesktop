# Copr "make_srpm" entry point for Mercurygram Desktop.
#
# Copr invokes:  make -f .copr/Makefile srpm outdir=<dir> spec=<path>
# with network access. We do NOT package the triggering commit: the 4th version
# component (N in X.Y.Z.N) is only assigned at release-tag time by
# Telegram/build/set_release_version.py and is absent from Telegram/build/version.
# Instead we package the latest published GitHub release, whose self-contained
# Mercurygram-<ver>-source-full.tar.gz already bundles every submodule.

OWNER   := Mercurygram
REPO    := mdesktop
outdir  ?= $(CURDIR)
specdir := $(CURDIR)/.copr

.ONESHELL:
.SHELLFLAGS := -ec
SHELL := /bin/bash

srpm:
	# Tooling (no-op when already present, e.g. a local dry-run as non-root).
	command -v curl >/dev/null 2>&1 || dnf -y install curl
	command -v rpmbuild >/dev/null 2>&1 || dnf -y install rpm-build
	mkdir -p "$(outdir)"
	# Resolve the latest non-prerelease, non-draft release tag (vX.Y.Z.N).
	tag=$$(curl -fsSL "https://api.github.com/repos/$(OWNER)/$(REPO)/releases/latest" \
		| sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
	test -n "$$tag" || { echo "could not resolve latest release tag" >&2; exit 1; }
	ver=$${tag#v}
	echo "Packaging Mercurygram $$ver ($$tag)"
	tarball="Mercurygram-$$ver-source-full.tar.gz"
	# Download the authoritative full-source tarball next to the spec sources.
	curl -fSL -o "$(outdir)/$$tarball" \
		"https://github.com/$(OWNER)/$(REPO)/releases/download/$$tag/$$tarball"
	# Stage spec (with the resolved version) and patch into the source dir.
	cp "$(specdir)/findprotobuf_fix.patch" "$(outdir)/"
	sed "s/^Version:.*/Version: $$ver/" "$(specdir)/mercurygram.spec" \
		> "$(outdir)/mercurygram.spec"
	rpmbuild -bs "$(outdir)/mercurygram.spec" \
		--define "_sourcedir $(outdir)" \
		--define "_specdir $(outdir)" \
		--define "_srcrpmdir $(outdir)"

.PHONY: srpm
