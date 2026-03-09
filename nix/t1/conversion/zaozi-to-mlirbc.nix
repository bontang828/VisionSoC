{
  lib,
  stdenvNoCC,

  circt-install,

  t1zaozi,
}:

{
  outputName,
  generatorClassName,
  parameterJson,
}:

stdenvNoCC.mkDerivation {
  name = outputName;

  nativeBuildInputs = [
    circt-install
  ];

  buildCommand = ''
    mkdir -p "$out"

    echo "[nix] Elaborating zaozi module with pre-existing parameter JSON"
    ${t1zaozi}/bin/t1zaozi ${generatorClassName} design ${parameterJson}

    echo "[nix] elaborate finish, collecting mlirbc output"
    mv *.mlirbc $out/
  '';
}
