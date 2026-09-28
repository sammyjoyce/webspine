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
  playwright-driver,
  epubcheck,
  dejavu_fonts,
  # hegel-cpp source and its prebuilt engine; both are needed only to run the test suite.
  hegelSource ? null,
  libhegel ? null,
  doCheck ? hegelSource != null && libhegel != null,
}:

let
  fontsConf = makeFontsConf { fontDirectories = [ dejavu_fonts ]; };
  # nixpkgs builds chromium only for Linux. On macOS use the Chrome for Testing
  # bundle that nixpkgs packages for Playwright, and point WEBSPINE_CHROMIUM at it.
  darwinChromium = "${playwright-driver.components.chromium}/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing";
  runtimeTools = [ epubcheck ] ++ lib.optional stdenv.hostPlatform.isLinux chromium;
  runtimeEnv = {
    FONTCONFIG_FILE = fontsConf;
  }
  // lib.optionalAttrs stdenv.hostPlatform.isDarwin { WEBSPINE_CHROMIUM = darwinChromium; };
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
  ]
  ++ lib.optional doCheck gtest;

  cmakeFlags = [ (lib.cmakeBool "WEBSPINE_BUILD_TESTS" doCheck) ];

  inherit doCheck;
  nativeCheckInputs = runtimeTools;
  env = lib.optionalAttrs doCheck (
    runtimeEnv
    // {
      HEGEL_CPP_SOURCE = "${hegelSource}";
      HEGEL_LIBHEGEL_LIBRARY = "${libhegel}/lib/libhegel_c${stdenv.hostPlatform.extensions.sharedLibrary}";
    }
  );
  preCheck = ''
    export HOME=$TMPDIR
  '';

  postFixup = ''
    wrapProgram $out/bin/webspine \
      ${
        lib.concatStringsSep " " (
          lib.mapAttrsToList (name: value: "--set-default ${name} ${lib.escapeShellArg value}") runtimeEnv
        )
      } \
      --prefix PATH : ${lib.makeBinPath runtimeTools}
  '';

  passthru = { inherit runtimeEnv runtimeTools; };

  meta = {
    description = "Turn rendered documentation sites into validated EPUB 3 books";
    homepage = "https://github.com/sammyjoyce/webspine";
    license = lib.licenses.mit;
    mainProgram = "webspine";
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
      "aarch64-darwin"
    ];
  };
}
