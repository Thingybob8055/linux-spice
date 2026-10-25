{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    qt6.wrapQtAppsHook
    makeWrapper
  ];

  buildInputs = with pkgs; [
    qt6.qtbase
    qt6.qtsvg
    qt6.qtwayland
    libngspice
    ngspice
    zlib
  ];

  # Tell the app where libngspice lives when run from the build tree.
  LINSPICE_NGSPICE_LIB = "${pkgs.libngspice}/lib/libngspice.so";
}
