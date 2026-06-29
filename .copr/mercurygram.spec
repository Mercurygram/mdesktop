%ifarch aarch64
    %global _lto_cflags %nil
%endif

# Internal directory name inside the source-full tarball (set by the release
# workflow's `git archive --prefix=tdesktop-<ver>-full/`).
%global fullname tdesktop

# Reducing debuginfo verbosity...
%global optflags %(echo %{optflags} | sed 's/-g /-g1 /')

Name: mercurygram
# Version is rewritten by .copr/Makefile to the latest published release
# (X.Y.Z.N). The placeholder below only matters for a manual local build.
Version: 0.0.0
Release: 1%{?dist}

# Application and 3rd-party modules licensing:
# * Mercurygram / Telegram Desktop - GPL-3.0-or-later with OpenSSL exception -- main tarball;
# * tg_owt - BSD-3-Clause AND BSD-2-Clause AND Apache-2.0 AND MIT AND LicenseRef-Fedora-Public-Domain -- linked dependency (separate package);
# * rlottie - LGPL-2.1-or-later AND FTL AND BSD-3-Clause -- static dependency;
# * cld3  - Apache-2.0 -- static dependency;
# * qt_functions.cpp - LGPL-3.0-only -- build-time dependency;
# * open-sans-fonts  - Apache-2.0 -- bundled font;
# * vazirmatn-fonts - OFL-1.1 -- bundled font.
License: GPL-3.0-or-later AND BSD-3-Clause AND BSD-2-Clause AND Apache-2.0 AND MIT AND LicenseRef-Fedora-Public-Domain AND LGPL-2.1-or-later AND FTL AND MPL-1.1 AND LGPL-3.0-only AND OFL-1.1
URL: https://github.com/Mercurygram/mdesktop
Summary: Mercurygram Desktop messaging app
Source0: %{url}/releases/download/v%{version}/Mercurygram-%{version}-source-full.tar.gz

Patch0: findprotobuf_fix.patch

# Telegram Desktop require more than 8 GB of RAM on linking stage.
# Disabling all low-memory architectures.
ExclusiveArch: x86_64 aarch64

BuildRequires: cmake(Microsoft.GSL)
BuildRequires: cmake(OpenAL)
BuildRequires: cmake(Qt6Concurrent)
BuildRequires: cmake(Qt6Core)
BuildRequires: cmake(Qt6Core5Compat)
BuildRequires: cmake(Qt6DBus)
BuildRequires: cmake(Qt6Gui)
BuildRequires: cmake(Qt6Network)
BuildRequires: cmake(Qt6OpenGL)
BuildRequires: cmake(Qt6OpenGLWidgets)
BuildRequires: cmake(Qt6Svg)
BuildRequires: cmake(Qt6WaylandClient)
BuildRequires: cmake(Qt6Widgets)
BuildRequires: cmake(fmt)
BuildRequires: cmake(range-v3)
BuildRequires: cmake(tg_owt)
BuildRequires: cmake(tl-expected)
BuildRequires: cmake(ada)

BuildRequires: pkgconfig(alsa)
BuildRequires: pkgconfig(gio-2.0)
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(glibmm-2.68) >= 2.76.0
BuildRequires: pkgconfig(gobject-2.0)
BuildRequires: pkgconfig(gobject-introspection-1.0)
BuildRequires: pkgconfig(hunspell)
BuildRequires: pkgconfig(jemalloc)
BuildRequires: pkgconfig(libavcodec)
BuildRequires: pkgconfig(libavfilter)
BuildRequires: pkgconfig(libavformat)
BuildRequires: pkgconfig(libavutil)
BuildRequires: pkgconfig(libcrypto)
BuildRequires: pkgconfig(liblz4)
BuildRequires: pkgconfig(liblzma)
BuildRequires: pkgconfig(libpulse)
BuildRequires: pkgconfig(libswresample)
BuildRequires: pkgconfig(libswscale)
BuildRequires: pkgconfig(libxxhash)
BuildRequires: pkgconfig(opus)
BuildRequires: pkgconfig(protobuf)
BuildRequires: pkgconfig(protobuf-lite)
BuildRequires: pkgconfig(rnnoise)
BuildRequires: pkgconfig(vpx)
BuildRequires: pkgconfig(wayland-client)
BuildRequires: pkgconfig(webkitgtk-6.0)
BuildRequires: pkgconfig(xcb)
BuildRequires: pkgconfig(xcb-keysyms)
BuildRequires: pkgconfig(xcb-record)
BuildRequires: pkgconfig(xcb-screensaver)

BuildRequires: boost-devel
BuildRequires: cmake
BuildRequires: desktop-file-utils
BuildRequires: ffmpeg-free-devel
BuildRequires: gcc
BuildRequires: gcc-c++
BuildRequires: libappstream-glib
# No libdispatch-devel: lib_crl selects its async backend by header probe
# (__has_include(<dispatch/dispatch.h>) wins before the bundled TooManyCooks
# path). With libdispatch present the dispatch backend is compiled in, but its
# link library is only wired when CMake's find_library(dispatch) also resolves
# inside the external_dispatch target -- which it does not do reliably on every
# arch, leaving undefined dispatch_* references at link time on aarch64. Keeping
# libdispatch out makes lib_crl fall through to the bundled TooManyCooks backend
# on all arches (matching the Flatpak build).
BuildRequires: libatomic
BuildRequires: libqrcodegencpp-devel
BuildRequires: libstdc++-devel
BuildRequires: minizip-compat-devel
BuildRequires: ninja-build
BuildRequires: python3
BuildRequires: qt6-qtbase-private-devel
BuildRequires: qt6-qtbase-static
BuildRequires: pkgconfig(openh264)
BuildRequires: cmake(KF6CoreAddons)
BuildRequires: cmake(tde2e)

Requires: hicolor-icon-theme
Requires: qt6-qtimageformats%{?_isa}
Requires: webkitgtk6.0%{?_isa}

# Virtual provides for bundled libraries...
Provides: bundled(cld3) = 3.0.13~gitb48dc46
Provides: bundled(kf6-kcoreaddons)
Provides: bundled(libtgvoip) = 2.4.4~git7c46f4c
Provides: bundled(open-sans-fonts) = 1.10
Provides: bundled(plasma-wayland-protocols) = 1.6.0
Provides: bundled(rlottie) = 0~git8c69fc2
Provides: bundled(vazirmatn-fonts) = 27.2.2
Provides: bundled(cppgir) = 0~git69ef481c
Provides: bundled(minizip) = 1.2.13

%description
Mercurygram Desktop is an unofficial, privacy and security focused fork of
Telegram Desktop. It is the desktop counterpart of the Mercurygram Android
client, built by rebasing Mercurygram patches on top of upstream tdesktop.

Telegram is a messaging app with a focus on speed and security: super fast,
simple and free. Messages sync seamlessly across any number of phones,
tablets or computers.

Mercurygram is not associated with Telegram FZ-LLC.

%prep
# Unpacking the Mercurygram source-full archive (internal prefix tdesktop-<ver>-full)...
%autosetup -n %{fullname}-%{version}-full -p1

# Unbundling libraries packaged by Fedora... (minizip stays bundled)
rm -rf Telegram/ThirdParty/{QR,dispatch,expected,fcitx-qt5,fcitx5-qt,hime,hunspell,jemalloc,kimageformats,lz4,nimf,range-v3,xxHash}

sed -i "/#include <openssl\/engine.h>/d" Telegram/SourceFiles/core/utils.cpp

%build
# Building Mercurygram Desktop using cmake against system libraries...
%cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_AR=%{_bindir}/gcc-ar \
    -DCMAKE_RANLIB=%{_bindir}/gcc-ranlib \
    -DCMAKE_NM=%{_bindir}/gcc-nm \
    -DTDESKTOP_API_ID=575730 \
    -DTDESKTOP_API_HASH=723c7927097f8487d229438af766e329 \
    -DDESKTOP_APP_USE_PACKAGED:BOOL=ON \
    -DDESKTOP_APP_USE_PACKAGED_FONTS:BOOL=OFF \
    -DDESKTOP_APP_DISABLE_WAYLAND_INTEGRATION:BOOL=OFF \
    -DDESKTOP_APP_DISABLE_X11_INTEGRATION:BOOL=OFF \
    -DDESKTOP_APP_DISABLE_CRASH_REPORTS:BOOL=ON \
    -DDESKTOP_APP_DISABLE_QT_PLUGINS:BOOL=ON
%cmake_build

%install
%cmake_install

%check
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/*.metainfo.xml
desktop-file-validate %{buildroot}%{_datadir}/applications/*.desktop

%files
%doc README.md changelog.txt
%license LICENSE LEGAL
%{_bindir}/Mercurygram
%{_datadir}/applications/it.belloworld.mercurygram.desktop
%{_datadir}/icons/hicolor/*/apps/it.belloworld.mercurygram*.png
%{_datadir}/icons/hicolor/*/apps/it.belloworld.mercurygram*.svg
%{_datadir}/dbus-1/services/it.belloworld.mercurygram.service
%{_metainfodir}/it.belloworld.mercurygram.metainfo.xml

%changelog
* Mon Jun 29 2026 Timothy Redaelli <timothy@fsfe.org> - 0.0.0-1
- Initial Mercurygram Desktop Copr package
