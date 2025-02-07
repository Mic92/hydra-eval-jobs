{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs, ... }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
    in
    {
      hydraJobs = import ./ci.nix { inherit pkgs; };

      legacyPackages.x86_64-linux = {
        brokenPkgs = {
          brokenPackage = throw "this is an evaluation error";
        };
        infiniteRecursionPkgs = {
          packageWithInfiniteRecursion =
            let
              recursion = [ recursion ];
            in
            derivation {
              inherit (pkgs) system;
              name = "drvB";
              recursiveAttr = recursion;
              builder = ":";
            };
        };
        success = {
          indirect_aggregate =
            pkgs.runCommand "indirect_aggregate"
              {
                _hydraAggregate = true;
                constituents = [
                  "anotherone"
                ];
              }
              ''
                touch $out
              '';
          direct_aggregate =
            pkgs.runCommand "direct_aggregate"
              {
                _hydraAggregate = true;
                constituents = [
                  self.hydraJobs.builtJob
                ];
              }
              ''
                touch $out
              '';
          mixed_aggregate =
            pkgs.runCommand "mixed_aggregate"
              {
                _hydraAggregate = true;
                constituents = [
                  self.hydraJobs.builtJob
                  "anotherone"
                ];
              }
              ''
                touch $out
              '';
          anotherone = pkgs.writeText "constituent" "text";
        };
        failures = {
          aggregate =
            pkgs.runCommand "aggregate"
              {
                _hydraAggregate = true;
                constituents = [
                  "doesntexist"
                  "doesnteval"
                ];
              }
              ''
                touch $out
              '';
          doesnteval = pkgs.writeText "constituent" (toString { });
        };
        glob1 = {
          constituentA = pkgs.runCommand "constituentA" { } "touch $out";
          constituentB = pkgs.runCommand "constituentB" { } "touch $out";
          aggregate = pkgs.runCommand "aggregate" {
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [ "*" ];
          } "touch $out";
        };
        cycle = {
          aggregate0 = pkgs.runCommand "aggregate0" {
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [ "aggregate1" ];
          } "touch $out";
          aggregate1 = pkgs.runCommand "aggregate1" {
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [ "aggregate0" ];
          } "touch $out";
        };
        glob2 = rec {
          packages = pkgs.recurseIntoAttrs {
            constituentA = pkgs.runCommand "constituentA" { } "touch $out";
            constituentB = pkgs.runCommand "constituentB" { } "touch $out";
          };
          aggregate0 = pkgs.runCommand "aggregate0" {
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [
              "packages.*"
            ];
          } "touch $out";
          aggregate1 = pkgs.runCommand "aggregate1" {
            _hydraAggregate = true;
            _hydraGlobConstituents = true;
            constituents = [
              "tests.*"
            ];
          } "touch $out";
          indirect_aggregate0 = pkgs.runCommand "indirect_aggregate0" {
            _hydraAggregate = true;
            constituents = [
              "aggregate0"
            ];
          } "touch $out";
          mix_aggregate0 = pkgs.runCommand "mix_aggregate0" {
            _hydraAggregate = true;
            constituents = [
              "aggregate0"
              packages.constituentA
            ];
          } "touch $out";
        };
      };
    };
}
