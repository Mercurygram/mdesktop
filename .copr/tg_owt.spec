%global debug_package %{nil}

%global commit0 07fb35bc33b3e79125a1beab7690e830f183f5d8
%global shortcommit0 %(c=%{commit0}; echo ${c:0:7})
%global date 20261013

# Git revision of libyuv...
%global commit1 04821d1e7d60845525e8db55c7bcd41ef5be9406
%global shortcommit1 %(c=%{commit1}; echo ${c:0:7})

# Git revision of crc32c...
%global commit2 21fc8ef30415a635e7351ffa0e5d5367943d4a94
%global shortcommit2 %(c=%{commit2}; echo ${c:0:7})

# Git revision of abseil-cpp...
%global commit3 d7aaad83b488fd62bd51c81ecf16cd938532cc0a
%global shortcommit3 %(c=%{commit3}; echo ${c:0:7})

# Git revision of libsrtp...
%global commit4 a566a9cfcd619e8327784aa7cff4a1276dc1e895
%global shortcommit4 %(c=%{commit4}; echo ${c:0:7})

Name: tg_owt
Version: 0
Release: 43.%{date}git%{shortcommit0}%{?dist}

# Library and 3rd-party bundled modules licensing:
# * tg_owt - BSD-3-Clause -- main tarball;
# * abseil-cpp - Apache-2.0 -- static dependency;
# * base64 - LicenseRef-Fedora-Public-Domain -- static dependency;
# * crc32c - BSD-3-Clause -- static dependency;
# * libsrtp - BSD-3-Clause -- static dependency;
# * libyuv - BSD-3-Clause -- static dependency;
# * openh264 - BSD-2-Clause -- static dependency;
# * pffft - BSD-3-Clause -- static dependency;
# * portaudio - MIT -- static dependency;
# * rnnoise - BSD-3-Clause -- static dependency;
# * sigslot - LicenseRef-Fedora-Public-Domain -- static dependency;
# * spl_sqrt_floor - LicenseRef-Fedora-Public-Domain -- static dependency.
License: BSD-3-Clause AND BSD-2-Clause AND Apache-2.0 AND MIT AND LicenseRef-Fedora-Public-Domain
Summary: WebRTC library for the Telegram messenger
URL: https://github.com/desktop-app/%{name}

Source0: %{url}/archive/%{commit0}/%{name}-%{shortcommit0}.tar.gz
Source1: https://gitlab.com/chromiumsrc/libyuv/-/archive/%{commit1}/libyuv-%{shortcommit1}.tar.gz
Source2: https://github.com/google/crc32c/archive/%{commit2}/crc32c-%{shortcommit2}.tar.gz
Source3: https://github.com/abseil/abseil-cpp/archive/%{commit3}/abseil-cpp-%{shortcommit3}.tar.gz
Source4: https://github.com/cisco/libsrtp/archive/%{commit4}/libsrtp-%{shortcommit4}.tar.gz

BuildRequires: pkgconfig(alsa)
BuildRequires: pkgconfig(epoxy)
BuildRequires: pkgconfig(gbm)
BuildRequires: pkgconfig(libavcodec)
BuildRequires: pkgconfig(libavfilter)
BuildRequires: pkgconfig(libavformat)
BuildRequires: pkgconfig(libavutil)
BuildRequires: pkgconfig(libdrm)
BuildRequires: pkgconfig(libjpeg)
BuildRequires: pkgconfig(libpipewire-0.3)
BuildRequires: pkgconfig(libpulse)
BuildRequires: pkgconfig(libswresample)
BuildRequires: pkgconfig(libswscale)
BuildRequires: pkgconfig(openssl)
BuildRequires: pkgconfig(opus)
BuildRequires: pkgconfig(vpx)
BuildRequires: pkgconfig(x11)
BuildRequires: pkgconfig(xcomposite)
BuildRequires: pkgconfig(xdamage)
BuildRequires: pkgconfig(xext)
BuildRequires: pkgconfig(xfixes)
BuildRequires: pkgconfig(xrandr)
BuildRequires: pkgconfig(xrender)
BuildRequires: pkgconfig(xtst)
BuildRequires: pkgconfig(openh264)

BuildRequires: cmake
BuildRequires: gcc
BuildRequires: gcc-c++
BuildRequires: ninja-build

# Disabling all low-memory architectures.
ExclusiveArch: x86_64 aarch64

%description
Special fork of the OpenWebRTC library for the Telegram messenger.

%package devel
Summary: Development files for %{name}
Provides: %{name}-static%{?_isa} = %{?epoch:%{epoch}:}%{version}-%{release}
Provides: bundled(abseil-cpp) = 20220623.1~git%{shortcommit3}
Provides: bundled(crc32c) = 1.1.0~git%{shortcommit2}
Provides: bundled(base64) = 0~git
Provides: bundled(fft) = 0~git
Provides: bundled(g711) = 1.1~git
Provides: bundled(g722) = 1.14~git
Provides: bundled(libsrtp) = 2.5.0~git%{shortcommit4}
Provides: bundled(libyuv) = 1872~git%{shortcommit1}
Provides: bundled(ooura) = 0~git
Provides: bundled(openh264) = 1.10.0~git6f26bce
Provides: bundled(pffft) = 0~git483453d
Provides: bundled(portaudio) = 0~git
Provides: bundled(rnnoise) = 0~git91ef40
Provides: bundled(sigslot) = 0~git
Provides: bundled(spl_sqrt_floor) = 0~git
Requires: pkgconfig(alsa)
Requires: pkgconfig(epoxy)
Requires: pkgconfig(gbm)
Requires: pkgconfig(libavcodec)
Requires: pkgconfig(libavfilter)
Requires: pkgconfig(libavformat)
Requires: pkgconfig(libavutil)
Requires: pkgconfig(libdrm)
Requires: pkgconfig(libjpeg)
Requires: pkgconfig(libpipewire-0.3)
Requires: pkgconfig(libpulse)
Requires: pkgconfig(libswresample)
Requires: pkgconfig(libswscale)
Requires: pkgconfig(openssl)
Requires: pkgconfig(opus)
Requires: pkgconfig(vpx)
Requires: pkgconfig(x11)
Requires: pkgconfig(xcomposite)
Requires: pkgconfig(xdamage)
Requires: pkgconfig(xext)
Requires: pkgconfig(xfixes)
Requires: pkgconfig(xrandr)
Requires: pkgconfig(xrender)
Requires: pkgconfig(xtst)

%description devel
%{summary}.

%prep
%autosetup -n %{name}-%{commit0} -p1
tar -xf %{SOURCE1} -C src/third_party/libyuv --strip=1
tar -xf %{SOURCE2} -C src/third_party/crc32c/src --strip=1
tar -xf %{SOURCE3} -C src/third_party/abseil-cpp --strip=1
tar -xf %{SOURCE4} -C src/third_party/libsrtp --strip=1

mkdir -p legal

entries=(
  "src/third_party/abseil-cpp:LICENSE:README.md:abseil-cpp"
  "src/third_party/crc32c/src:LICENSE:README.md:crc32c"
  "src/third_party/libsrtp:LICENSE:README.md:libsrtp"
  "src/third_party/libyuv:LICENSE:README.chromium:libyuv"
  "src/third_party/pffft:LICENSE:README.chromium:pffft"
  "src/third_party/rnnoise:COPYING:README.chromium:rnnoise"
  "src/common_audio/third_party/ooura:LICENSE:README.chromium:ooura"
  "src/common_audio/third_party/spl_sqrt_floor:LICENSE:README.chromium:spl_sqrt_floor"
  "src/modules/third_party/fft:LICENSE:README.chromium:fft"
  "src/modules/third_party/g711:LICENSE:README.chromium:g711"
  "src/modules/third_party/g722:LICENSE:README.chromium:g722"
  "src/modules/third_party/portaudio:LICENSE:README.chromium:portaudio"
  "src/rtc_base/third_party/base64:LICENSE:README.chromium:base64"
  "src/rtc_base/third_party/sigslot:LICENSE:README.chromium:sigslot"
)

for e in "${entries[@]}"; do
  IFS=: read -r path lic readme name <<< "$e"
  cp -f -p "$path/$lic" "legal/LICENSE.$name"
  cp -f -p "$path/$readme" "legal/README.$name"
done

cp -f -p src/third_party/libyuv/PATENTS legal/PATENTS.libyuv

%build
# CMAKE_BUILD_TYPE should always be Release due to some hardcoded checks.
# Warning: Building as a shared library is not supported by upstream.
%cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON \
    -DBUILD_SHARED_LIBS:BOOL=OFF \
    -DTG_OWT_USE_PROTOBUF:BOOL=OFF \
    -DTG_OWT_PACKAGED_BUILD:BOOL=ON
%cmake_build

%install
%cmake_install

%files devel
%doc src/AUTHORS src/OWNERS legal/README.*
%license LICENSE src/PATENTS legal/LICENSE.* legal/PATENTS.*
%{_includedir}/%{name}/
%{_libdir}/cmake/%{name}/
%{_libdir}/lib%{name}.a

%changelog
* Wed Oct 21 2026 Timothy Redaelli <timothy.redaelli@gmail.com> - 0-43.20261013git07fb35b
- Switched to 07fb35bc33b3e79125a1beab7690e830f183f5d8 snapshot (Mercurygram 7.3.2.1 pin).

* Thu Oct 15 2026 Timothy Redaelli <timothy.redaelli@gmail.com> - 0-42.20261007gitd1cf250
- Switched to d1cf250ea73de26c4c1f0a3c8173eb2648efbb04 snapshot (Mercurygram 7.3.1.1 pin).
- Dropped the GCC 16 patch, merged upstream.
- Dropped ffmpeg-devel, not in Fedora; the libav* pkgconfig deps cover it.

* Tue Aug 04 2026 Leigh Scott <leigh123linux@gmail.com> - 0-41.20260803git19d51d3
- Switched to 19d51d3c19632a63fdbe17c62f10332d978cb940 snapshot.
- Drop unused patches

* Sun Aug 02 2026 RPM Fusion Release Engineering <leigh123linux@rpmfusion.org> - 0-40.20250501gitc4192e8
- Rebuilt for https://fedoraproject.org/wiki/Fedora_45_Mass_Rebuild

* Mon Feb 02 2026 RPM Fusion Release Engineering <sergiomb@rpmfusion.org> - 0-39.20250501gitc4192e8
- Rebuilt for https://fedoraproject.org/wiki/Fedora_44_Mass_Rebuild

* Sun Jul 27 2025 RPM Fusion Release Engineering <sergiomb@rpmfusion.org> - 0-38.20250501gitc4192e8
- Rebuilt for https://fedoraproject.org/wiki/Fedora_43_Mass_Rebuild

* Mon May 05 2025 Vasiliy Glazov <vascom2@gmail.com> - 0-37.20250501gitc4192e8
- Switched to c4192e8e2e10ccb72704daa79fa108becfa57b01 snapshot.

* Tue Jan 28 2025 RPM Fusion Release Engineering <sergiomb@rpmfusion.org> - 0-36.20240730gite9d103e
- Rebuilt for https://fedoraproject.org/wiki/Fedora_42_Mass_Rebuild

* Thu Aug 01 2024 Vasiliy Glazov <vascom2@gmail.com> - 0-35.20240730gite9d103e
- Switched to e9d103e2480e0983bf464debc371b049cdd83648 snapshot.

* Sun Feb 04 2024 RPM Fusion Release Engineering <sergiomb@rpmfusion.org> - 0-34.20230921git3bb3d75
- Rebuilt for https://fedoraproject.org/wiki/Fedora_40_Mass_Rebuild

* Mon Sep 25 2023 Vasiliy Glazov <vascom2@gmail.com> - 0-32.20230501git3bb3d75
- Switched to 3bb3d757681e6cc5135aec6529a753dc3dcdcfb9 snapshot.

* Wed Aug 02 2023 RPM Fusion Release Engineering <sergiomb@rpmfusion.org> - 0-32.20230501gitdcb5069
- Rebuilt for https://fedoraproject.org/wiki/Fedora_39_Mass_Rebuild

* Sun May 21 2023 Vitaly Zaitsev <vitaly@easycoding.org> - 0-31.20230501gitdcb5069
- Switched to dcb5069ff76bd293e86928804208737e6cee2ccc snapshot.

* Sat Apr 22 2023 Vitaly Zaitsev <vitaly@easycoding.org> - 0-30.20230418gitfe316b0
- Switched to fe316b0c5a155cceb2ddecee70d7b582cadfa225 snapshot.

* Mon Mar 20 2023 Vitaly Zaitsev <vitaly@easycoding.org> - 0-29.20230314git1a18da2
- Switched to 1a18da2ed4d5ce134e984d1586b915738e0da257 snapshot.
- Switched to modern ffmpeg and OpenSSL.
