{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  makeWrapper,
  makeFontsConf,
  cli11,
  curl,
  gtest,
  libxml2,
  libzip,
  nlohmann_json,
  openssl,
  vips,
  chromium,
  epubcheck,
  dejavu_fonts,
  # hegel-cpp source and its prebuilt engine; both are needed only to run the test suite.
  hegelSource ? null,
  libhegel ? null,
  doCheck ? hegelSource != null && libhegel != null,
}:

let
  fontsConf = makeFontsConf { fontDirectories = [ dejavu_fonts ]; };
  runtimeTools = [
    chromium
    epubcheck
  ];
in
stdenv.mkDerivation {
  pname = "webspine";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ./..;
    fileset = lib.fileset.unions [
      ../CMakeLists.txt
      ../src
      ../tests
    ];
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    makeWrapper
  ];
  buildInputs = [
    cli11
    curl
    libxml2
    libzip
    nlohmann_json
    openssl
    vips
  ] ++ lib.optional doCheck gtest;

  cmakeFlags = [ (lib.cmakeBool "WEBSPINE_BUILD_TESTS" doCheck) ];

  inherit doCheck;
  nativeCheckInputs = runtimeTools;
  env = lib.optionalAttrs doCheck {
    HEGEL_CPP_SOURCE = "${hegelSource}";
    HEGEL_LIBHEGEL_LIBRARY = "${libhegel}/lib/libhegel_c.so";
    FONTCONFIG_FILE = fontsConf;
  };
  preCheck = ''
    export HOME=$TMPDIR
  '';

  postFixup = ''
    wrapProgram $out/bin/webspine \
      --set-default FONTCONFIG_FILE ${fontsConf} \
      --prefix PATH : ${lib.makeBinPath runtimeTools}
  '';

  passthru = { inherit fontsConf runtimeTools; };

  meta = {
    description = "Turn rendered documentation sites into validated EPUB 3 books";
    homepage = "https://github.com/sammyjoyce/webspine";
    mainProgram = "webspine";
    platforms = lib.platforms.linux;
  };
}
