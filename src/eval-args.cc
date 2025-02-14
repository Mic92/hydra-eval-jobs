#include <cstdio>
#include <cstdlib>
#include <nix/args.hh>
#include <nix/file-system.hh>
#include <nix/flake/flake.hh>
#include <nix/flake/lockfile.hh>
#include <nix/canon-path.hh>
#include <nix/common-args.hh>
#include <nix/common-eval-args.hh>
#include <nix/source-accessor.hh>
#include <nix/flake/flakeref.hh>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>

#include "eval-args.hh"

MyArgs::MyArgs() : MixCommonArgs("nix-eval-jobs") {
    addFlag({
        .longName = "help",
        .description = "show usage information",
        .handler = {[&]() {
            std::cout << "USAGE: nix-eval-jobs [options] expr\n\n";
            for (const auto &[name, flag] : longFlags) {
                if (hiddenCategories.contains(flag->category)) {
                    continue;
                }
                std::cout << "  --" << std::left << std::setw(20) << name << " "
                          << flag->description << "\n";
            }

            ::exit(0); // NOLINT(concurrency-mt-unsafe)
        }},
    });

    addFlag({.longName = "impure",
             .description = "allow impure expressions",
             .handler = {&impure, true}});

    addFlag({.longName = "force-recurse",
             .description = "force recursion (don't respect recurseIntoAttrs)",
             .handler = {&forceRecurse, true}});

    addFlag({.longName = "gc-roots-dir",
             .description = "garbage collector roots directory",
             .labels = {"path"},
             .handler = {&gcRootsDir}});

    addFlag({.longName = "workers",
             .description = "number of evaluate workers",
             .labels = {"workers"},
             .handler = {
                 [this](const std::string &s) { nrWorkers = std::stoi(s); }}});

    addFlag({.longName = "max-memory-size",
             .description = "maximum evaluation memory size in megabyte "
                            "(4GiB per worker by default)",
             .labels = {"size"},
             .handler = {[this](const std::string &s) {
                 maxMemorySize = std::stoi(s);
             }}});

    addFlag({.longName = "flake",
             .description = "build a flake",
             .handler = {&flake, true}});

    addFlag({.longName = "meta",
             .description = "include derivation meta field in output",
             .handler = {&meta, true}});

    addFlag(
        {.longName = "constituents",
         .description =
             "whether to evaluate constituents for Hydra's aggregate feature",
         .handler = {&constituents, true}});

    addFlag(
        {.longName = "check-cache-status",
         .description =
             "Check if the derivations are present locally or in "
             "any configured substituters (i.e. binary cache). The "
             "information "
             "will be exposed in the `cacheStatus` field of the JSON output.",
         .handler = {&checkCacheStatus, true}});

    addFlag({.longName = "show-input-drvs",
             .description =
                 "Show input derivations in the output for each derivation. "
                 "This is useful to get direct dependencies of a derivation.",
             .handler = {&showInputDrvs, true}});

    addFlag(
        {.longName = "show-trace",
         .description = "print out a stack trace in case of evaluation errors",
         .handler = {&showTrace, true}});

    addFlag({.longName = "expr",
             .shortName = 'E',
             .description = "treat the argument as a Nix expression",
             .handler = {&fromArgs, true}});

    addFlag(
        {.longName = "apply",
         .description = "apply the derivation to the provided Nix expression",
         .labels = {"expr"},
         .handler = {&applyExpr}});

    // usually in MixFlakeOptions
    addFlag({
        .longName = "override-input",
        .description =
            "Override a specific flake input (e.g. `dwarffs/nixpkgs`).",
        .category = category,
        .labels = {"input-path", "flake-url"},
        .handler = {[&](const std::string &inputPath,
                        const std::string &flakeRef) {
            // overriden inputs are unlocked
            lockFlags.allowUnlocked = true;
            lockFlags.inputOverrides.insert_or_assign(
                nix::flake::parseInputAttrPath(inputPath),
                nix::parseFlakeRef(nix::fetchSettings, flakeRef,
                                   nix::absPath(std::filesystem::path(".")),
                                   true));
        }},
    });

    addFlag({.longName = "reference-lock-file",
             .description = "Read the given lock file instead of `flake.lock` "
                            "within the top-level flake.",
             .category = category,
             .labels = {"flake-lock-path"},
             .handler = {[&](const std::string &lockFilePath) {
                 lockFlags.referenceLockFilePath = {
                     nix::getFSSourceAccessor(),
                     nix::CanonPath(nix::absPath(lockFilePath))};
             }},
             .completer = completePath});

    expectArg("expr", &releaseExpr);
}

void MyArgs::parseArgs(char **argv, int argc) {
    parseCmdline(nix::argvToStrings(argc, argv), false);
}
