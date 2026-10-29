Name:           qtparted
Version:        1.0.0
Release:        1%{?dist}
Summary:        Partition editor for Qt 6 based on libparted
License:        GPL-2.0-or-later
URL:            https://github.com/zanac/qtparted
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(Qt6Core) >= 6.4
BuildRequires:  pkgconfig(Qt6Widgets)
BuildRequires:  qt6-linguist
BuildRequires:  pkgconfig(libparted)
BuildRequires:  pkgconfig(libparted-fs-resize)
BuildRequires:  pkgconfig(blkid)
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib
Requires:       polkit
Requires:       qt6-qtsvg
Recommends:     e2fsprogs dosfstools mtools ntfs-3g btrfs-progs xfsprogs exfatprogs cryptsetup lvm2

%description
QtParted creates, deletes, resizes, moves, copies, checks and formats disk
partitions, opens and resizes LUKS encrypted volumes, mounts file systems and
activates swap and LVM Volume Groups. Changes are planned on screen and applied
together, with a detailed log.

%prep
%autosetup

%build
%cmake -G Ninja -DQTPARTED_BUILD_TESTS=OFF
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.zanac.qtparted.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/io.github.zanac.qtparted.metainfo.xml

%files
%license COPYING
%{_datadir}/doc/qtparted/
%{_bindir}/qtparted
%{_libexecdir}/qtparted/
%{_datadir}/applications/io.github.zanac.qtparted.desktop
%{_metainfodir}/io.github.zanac.qtparted.metainfo.xml
%{_datadir}/polkit-1/actions/io.github.zanac.qtparted.policy
%{_datadir}/icons/hicolor/*/apps/qtparted.png
%{_mandir}/man1/qtparted.1*

%changelog
* Thu Oct 29 2026 Vanni Brutto <zanac4ever@gmail.com> - 1.0.0-1
- Qt 6 rewrite
