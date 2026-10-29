# Packaging

- **Debian / Ubuntu** (.deb) and **Fedora / openSUSE** (.rpm), built with CPack from a
  normal build directory:

  ```sh
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DQTPARTED_BUILD_TESTS=OFF
  cmake --build build
  cd build && cpack -G DEB      # or: cpack -G RPM (needs rpmbuild)
  ```

  The .deb gets its library dependencies from `dpkg-shlibdeps`, depends on pkexec and
  the Qt SVG plugin, and recommends the common file system tools.

- **Arch Linux**: `arch/PKGBUILD` (builds the `v$pkgver` tag).
- **Fedora (rpmbuild / COPR)**: `rpm/qtparted.spec`.

The program is installed in `libexec/qtparted/` and started by the `qtparted`
launcher through pkexec; the polkit policy names the program's path, so a package
must keep both from the same build.

## Releases

`.github/workflows/release.yml` makes a release: run it from the Actions tab on
`main` with the tag (e.g. `v1.0.0`). It checks that `CMakeLists.txt` has that
version and that `doc/release-notes/<tag>.md` exists, builds the .deb and the source
tarball, creates the tag and publishes the release with those files.
