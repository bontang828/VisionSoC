{
  lib,
  stdenvNoCC,
  circt-install,
}:

{
  outputName,
  mlirbcs,
  baseCircuit,
}:

stdenvNoCC.mkDerivation {
  name = outputName;

  nativeBuildInputs = [ circt-install ];

  buildCommand = ''
    mkdir -p $out

    firld \
      --base-circuit ${baseCircuit} \
      --no-mangle \
      --emit-bytecode \
      -o $out/$name \
      ${lib.concatMapStringsSep " " (m: "${m}/*.mlirbc") mlirbcs}
  '';
}
