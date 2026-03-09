{
  callPackage,
  publishMillJar,
  git,
  makeSetupHook,
  writeText,
  lib,
  newScope,
  circt-install,
  mlir-install,
  zaozi-src,
  chisel-src,
  jextract-21,
  runCommand,
  writeShellApplication,
  millVersions,
  mill-ivy-fetcher,
}:

let
  inherit (millVersions) mill_0_12_9 mill_1_1_2;
  publishMillJar-0_12_9 = publishMillJar.override { mill = mill_0_12_9; };
  publishMillJar-1_1_2 = publishMillJar.override { mill = mill_1_1_2; };
in
lib.makeScope newScope (scope: {

  ivy-chisel = publishMillJar-1_1_2 {
    name = "chisel-snapshot";
    src = chisel-src;

    lockFile = ../locks/chisel-lock.nix;

    publishTargets = [
      "unipublish[2.13]"
    ];

    nativeBuildInputs = [
      # chisel requires git to generate version
      git
    ];

    preBuild = ''
      # Fix mill JVM version detection, use JVM version of the system
      sed -i '1i //| mill-jvm-version: system' build.mill
    '';

    passthru.bump = writeShellApplication {
      name = "bump-chisel-mill-lock";

      runtimeInputs = [
        mill_1_1_2
        mill-ivy-fetcher
      ];

      text = ''
        mif run -p "${chisel-src}" -o ./dependencies/locks/chisel-lock.nix "$@"
      '';
    };
  };

  ivy-zaozi = publishMillJar-1_1_2 {
    name = "zaozi-snapshot";
    src = zaozi-src;

    publishTargets = [
      "mlirlib"
      "circtlib"
      "omlib"
      "zaozi"
      "stdlib"
      "decoder"
    ];

    env = {
      CIRCT_INSTALL_PATH = circt-install;
      MLIR_INSTALL_PATH = mlir-install;
      JEXTRACT_INSTALL_PATH = jextract-21;
      JAVA_TOOL_OPTIONS = "-Djextract.decls.per.header=65535 --enable-preview";
    };

    lockFile = ../locks/zaozi-lock.nix;

    passthru.bump = writeShellApplication {
      name = "bump-zaozi-mill-lock";

      runtimeInputs = [
        mill_1_1_2
        mill-ivy-fetcher
      ];

      text = ''
        mif run -p "${zaozi-src}" -o ./dependencies/locks/zaozi-lock.nix "$@"
      '';
    };

    nativeBuildInputs = [ git ];
  };

  ivy-arithmetic = publishMillJar-0_12_9 {
    name = "arithmetic-snapshot";
    src = ../arithmetic;

    publishTargets = [
      "arithmetic[snapshot]"
    ];

    buildInputs = [
      scope.ivy-chisel.setupHook
    ];

    lockFile = ../locks/arithmetic-mill-lock.nix;

    passthru.bump = writeShellApplication {
      name = "bump-zaozi-mill-lock";

      runtimeInputs = [
        mill_0_12_9
        mill-ivy-fetcher
      ];

      text = ''
        ivyLocal="${scope.ivy-chisel}"
        export JAVA_TOOL_OPTIONS="''${JAVA_TOOL_OPTIONS:-} -Dcoursier.ivy.home=$ivyLocal -Divy.home=$ivyLocal"

        mif run -p "${../arithmetic}" \
          --targets "arithmetic[snapshot]" \
          -o ./dependencies/locks/arithmetic-mill-lock.nix "$@"
      '';
    };
  };

  ivy-chisel-interface = publishMillJar-0_12_9 {
    name = "chiselInterface-snapshot";
    src = ../chisel-interface;

    publishTargets = [
      "jtag[snapshot]"
      "axi4[snapshot]"
      "dwbb[snapshot]"
    ];

    nativeBuildInputs = [ git ];

    lockFile = ../locks/chisel-interface-lock.nix;

    buildInputs = [
      scope.ivy-chisel.setupHook
    ];

    passthru.bump = writeShellApplication {
      name = "bump-zaozi-mill-lock";

      runtimeInputs = [
        mill_0_12_9
        mill-ivy-fetcher
      ];

      text = ''
        ivyLocal="${scope.ivy-chisel}"
        export JAVA_TOOL_OPTIONS="''${JAVA_TOOL_OPTIONS:-} -Dcoursier.ivy.home=$ivyLocal -Divy.home=$ivyLocal"

        mif run -p "${../chisel-interface}" \
          --targets "jtag[snapshot]" \
          --targets "axi4[snapshot]" \
          --targets "dwbb[snapshot]" \
          -o ./dependencies/locks/chisel-interface-lock.nix "$@"
      '';
    };
  };

  ivy-rvdecoderdb = publishMillJar-0_12_9 rec {
    name = "rvdecoderdb-snapshot";
    src = ../rvdecoderdb;

    publishTargets = [
      "rvdecoderdb.jvm"
    ];

    lockFile = ../locks/rvdecoderdb-lock.nix;

    nativeBuildInputs = [
      # rvdecoderdb requires git to generate version
      git
    ];

    passthru.bump = writeShellApplication {
      name = "bump-rvdecoderdb-mill-lock";
      runtimeInputs = [
        mill_0_12_9
        mill-ivy-fetcher
      ];
      text = ''
        mif run \
          --targets 'rvdecoderdb.jvm' \
          -p "${../rvdecoderdb}" -o ./dependencies/locks/rvdecoderdb-lock.nix "$@"
      '';
    };
  };

  ivy-rvdecoderdb3 = publishMillJar-0_12_9 rec {
    name = "rvdecoderdb-3-snapshot";
    src = zaozi-src;

    publishTargets = [
      "rvdecoderdb"
    ];

    lockFile = ../locks/rvdecoderdb-3-lock.nix;

    nativeBuildInputs = [
      # rvdecoderdb requires git to generate version
      git
    ];

    passthru.bump = writeShellApplication {
      name = "bump-rvdecoderdb-3-mill-lock";
      runtimeInputs = [
        mill_0_12_9
        mill-ivy-fetcher
      ];
      text = ''
        mif run \
          --targets 'rvdecoderdb' \
          -p "${src}" -o ./dependencies/locks/rvdecoderdb-3-lock.nix "$@"
      '';
    };
  };

  ivy-hardfloat = publishMillJar-0_12_9 rec {
    name = "hardfloat-snapshot";
    src = ../berkeley-hardfloat;

    publishTargets = [
      "hardfloat[snapshot]"
    ];

    buildInputs = [
      scope.ivy-chisel.setupHook
    ];

    lockFile = ../locks/berkeley-hardfloat-lock.nix;

    nativeBuildInputs = [
      # hardfloat requires git to generate version
      git
    ];

    passthru.bump = writeShellApplication {
      name = "bump-hardfloat-mill-lock";
      runtimeInputs = [
        mill_0_12_9
        mill-ivy-fetcher
      ];
      text = ''
        ivyLocal="${scope.ivy-chisel}"
        export JAVA_TOOL_OPTIONS="''${JAVA_TOOL_OPTIONS:-} -Dcoursier.ivy.home=$ivyLocal -Divy.home=$ivyLocal"

        mif run \
          --targets 'hardfloat[snapshot]' \
          -p "${src}" -o ./dependencies/locks/berkeley-hardfloat-lock.nix "$@"
      '';
    };
  };

  riscv-opcodes = makeSetupHook { name = "setup-riscv-opcodes-src"; } (
    writeText "setup-riscv-opcodes-src.sh" ''
      setupRiscvOpcodes() {
        mkdir -p dependencies
        ln -sfT "${../riscv-opcodes}" "dependencies/riscv-opcodes"
      }
      prePatchHooks+=(setupRiscvOpcodes)
    ''
  );

  ivyLocalRepo =
    runCommand "build-coursier-env"
      {
        buildInputs = with scope; [
          ivy-arithmetic.setupHook
          ivy-chisel.setupHook
          ivy-zaozi.setupHook
          ivy-chisel-interface.setupHook
          ivy-rvdecoderdb.setupHook
          ivy-hardfloat.setupHook
        ];
      }
      ''
        runHook preUnpack
        runHook postUnpack
        cp -r "$NIX_COURSIER_DIR" "$out"
      '';
})
