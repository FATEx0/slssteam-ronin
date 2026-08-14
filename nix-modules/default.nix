{
  rev,
  lib,
  pkgs,
}:
pkgs.pkgsi686Linux.stdenv.mkDerivation {
  pname = "SLSsteam";
  version = "${rev}";
  src = ../.;

  nativeBuildInputs = with pkgs; [
    pkg-config
    makeWrapper
    jq
  ];

  buildInputs = with pkgs.pkgsi686Linux; [
    openssl
    curl
  ];

  postPatch = ''
    substituteInPlace src/log.hpp \
      --replace-fail "notify-send" "${lib.getExe pkgs.libnotify}"
  '';

  buildPhase = ''
    make bin/SLSsteam.so bin/library-inject.so bin/sls-prelaunch
    ${pkgs.stdenv.cc}/bin/g++ -O2 -std=c++20 -Wall -Wextra -Wpedantic \
      -I${pkgs.openssl.dev}/include tools/slssteam-control.cpp \
      -L${pkgs.openssl.out}/lib -Wl,-rpath,${pkgs.openssl.out}/lib \
      -lcrypto -o bin/slssteam-control
  '';

  installPhase = ''
    mkdir -p $out/
    cp bin/SLSsteam.so $out/
    cp bin/library-inject.so $out/
    cp bin/sls-prelaunch $out/
    cp bin/slssteam-control $out/
  '';

  meta = {
    description = "Steamclient Modification for Linux";
    homepage = "https://github.com/AceSLS/SLSsteam";
    license = lib.licenses.agpl3Only;
    platforms = lib.platforms.linux;
  };
}
