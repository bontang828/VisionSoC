{
  lib,
  getTestRequiredFeatures,
  linkerScript,
  makeBuilder,
  findAndBuild,
  t1main,
}:

let
  builder = makeBuilder { casePrefix = "vision_program"; };
  emurtSrc = ../emurt/emurt.c;
  build =
    { caseName, sourcePath }:
    builder {
      inherit caseName;

      src = sourcePath;

      passthru.featuresRequired = getTestRequiredFeatures sourcePath;

      buildPhase = ''
        runHook preBuild

        $CC -T${linkerScript} \
          ${caseName}.c \
          ${emurtSrc} \
          ${t1main} \
          -I${../emurt} \
          -o $pname.elf

        runHook postBuild
      '';

      meta.description = "test case '${caseName}', vision_program with emurt printf support";
    };
in
findAndBuild ./. build
