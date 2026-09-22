# Third-party components

NekoBeat itself is licensed under GPL-3.0-or-later (see [LICENSE](LICENSE)). The source
package contains only NekoBeat's own code.

The **portable package** also distributes the third-party binaries listed below. Their
licences apply to those files. This file is copied into the portable package as
`THIRD_PARTY_NOTICES.txt`, together with the full GPL text as `LICENSE.txt`.

## Qt 6

- Components: Qt 6.9.3 Core, Gui, Widgets, Network, SerialPort, OpenGL, OpenGLWidgets and
  Svg, plus the platform, style, image format, icon engine, TLS and network information
  plugins.
- Licence: GNU Lesser General Public License v3 (LGPL-3.0), or a commercial Qt licence.
- Source: <https://www.qt.io/> and <https://code.qt.io/>
- The Qt libraries are linked **dynamically**. You are free to replace any Qt DLL in the
  portable package with your own build of the same version; NekoBeat does not statically
  link Qt.
- Licence text: <https://www.gnu.org/licenses/lgpl-3.0.html>

## libmpv

- Component: `libmpv-2.dll`, taken from the build
  `mpv-dev-x86_64-20260903-git-69e63f425a`.
- Licence: GNU General Public License v2 or later (GPLv2+), which is the default licence
  of the mpv core. NekoBeat and mpv are therefore both free software under the GPL.
- Source: <https://github.com/shinchiro/mpv-winbuild-cmake> (builds of
  <https://github.com/mpv-player/mpv>)
- The library is loaded dynamically at runtime.
- Licence text: <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>, and the full
  GPL-3.0 text shipped in this package.

## MinGW-w64 runtime

- Components: `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`.
- Licence: GPLv3 or later **with the GCC Runtime Library Exception** for `libgcc` and
  `libstdc++`, and permissive terms for `libwinpthread`. The runtime exception is what
  allows these files to be redistributed with a program built by MinGW-w64.
- Source: <https://gcc.gnu.org/> and <https://www.mingw-w64.org/>
- Licence text: <https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html>

## Other files collected by windeployqt

- `D3Dcompiler_47.dll` is redistributed by Microsoft and shipped for the Windows platform
  plugin.
- `opengl32sw.dll` is the Mesa software OpenGL fallback, distributed under permissive
  terms.
- Both are added by Qt's `windeployqt`; they are not part of NekoBeat's own code.

## libmpv source availability

The GPL requires that the complete corresponding source of the GPL-covered binaries be
available. `libmpv-2.dll` is an unmodified build of mpv, published at the project links
above and reproducible from them; NekoBeat's own source corresponds to the source package
released alongside this one.
