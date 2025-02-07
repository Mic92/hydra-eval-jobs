#!/usr/bin/env python3

import json
import subprocess
import os
import pytest
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any, Dict, List

TEST_ROOT = Path(__file__).parent.resolve()
PROJECT_ROOT = TEST_ROOT.parent
BIN = PROJECT_ROOT.joinpath("build", "src", "nix-eval-jobs")


def check_gc_root(gcRootDir: str, drvPath: str):
    """
    Make sure the expected GC root exists in the given dir
    """
    link_name = os.path.basename(drvPath)
    symlink_path = os.path.join(gcRootDir, link_name)
    assert os.path.islink(symlink_path) and drvPath == os.readlink(symlink_path)


def common_test(extra_args: List[str]) -> List[Dict[str, Any]]:
    with TemporaryDirectory() as tempdir:
        cmd = [str(BIN), "--gc-roots-dir", tempdir, "--meta"] + extra_args
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            check=True,
            stdout=subprocess.PIPE,
        )

        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 5

        built_job = results[0]
        assert built_job["attr"] == "builtJob"
        assert built_job["name"] == "job1"
        assert built_job["outputs"]["out"].startswith("/nix/store")
        assert built_job["drvPath"].endswith(".drv")
        assert built_job["meta"]["broken"] is False

        dotted_job = results[1]
        assert dotted_job["attr"] == '"dotted.attr"'
        assert dotted_job["attrPath"] == ["dotted.attr"]

        package_with_deps = results[2]
        assert package_with_deps["attr"] == "package-with-deps"
        assert package_with_deps["name"] == "package-with-deps"

        recurse_drv = results[3]
        assert recurse_drv["attr"] == "recurse.drvB"
        assert recurse_drv["name"] == "drvB"

        substituted_job = results[4]
        assert substituted_job["attr"] == "substitutedJob"
        assert substituted_job["name"].startswith("nix-")
        assert substituted_job["meta"]["broken"] is False

        assert len(list(Path(tempdir).iterdir())) == 4
        return results


def test_flake() -> None:
    results = common_test(["--flake", ".#hydraJobs"])
    for result in results:
        assert "isCached" not in result  # legacy
        assert "cacheStatus" not in result
        assert "neededBuilds" not in result
        assert "neededSubstitutes" not in result


def test_query_cache_status() -> None:
    results = common_test(["--flake", ".#hydraJobs", "--check-cache-status"])
    # FIXME in the nix sandbox we cannot query binary caches
    # this would need some local one
    for result in results:
        assert "isCached" in result  # legacy
        assert "cacheStatus" in result
        assert "neededBuilds" in result
        assert "neededSubstitutes" in result


def test_expression() -> None:
    results = common_test(["ci.nix"])
    for result in results:
        assert "isCached" not in result  # legacy
        assert "cacheStatus" not in result

    with open(TEST_ROOT.joinpath("assets/ci.nix"), "r") as ci_nix:
        common_test(["-E", ci_nix.read()])


def test_input_drvs() -> None:
    results = common_test(["ci.nix", "--show-input-drvs"])
    for result in results:
        assert "inputDrvs" in result


def test_eval_error() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            "--flake",
            ".#legacyPackages.x86_64-linux.brokenPkgs",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        attrs = json.loads(res.stdout)
        assert attrs["attr"] == "brokenPackage"
        assert "this is an evaluation error" in attrs["error"]


def test_constituents() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            "--flake",
            ".#legacyPackages.x86_64-linux.success",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 4
        child = results[0]
        assert child["attr"] == "anotherone"
        direct = results[1]
        assert direct["attr"] == "direct_aggregate"
        indirect = results[2]
        assert indirect["attr"] == "indirect_aggregate"
        mixed = results[3]
        assert mixed["attr"] == "mixed_aggregate"

        def absent_or_empty(f: str, d: dict) -> bool:
            return f not in d or len(d[f]) == 0

        assert absent_or_empty("namedConstituents", direct)
        assert absent_or_empty("namedConstituents", indirect)
        assert absent_or_empty("namedConstituents", mixed)

        assert direct["constituents"][0].endswith("-job1.drv")

        assert indirect["constituents"][0] == child["drvPath"]

        assert mixed["constituents"][0].endswith("-job1.drv")
        assert mixed["constituents"][1] == child["drvPath"]

        assert "error" not in direct
        assert "error" not in indirect
        assert "error" not in mixed

        check_gc_root(tempdir, direct["drvPath"])
        check_gc_root(tempdir, indirect["drvPath"])
        check_gc_root(tempdir, mixed["drvPath"])


def test_constituents_all() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            "--flake",
            ".#legacyPackages.x86_64-linux.glob1",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 3
        assert list(map(lambda x: x["name"], results)) == [
            "constituentA",
            "constituentB",
            "aggregate",
        ]
        aggregate = results[2]
        assert len(aggregate["constituents"]) == 2
        assert aggregate["constituents"][0].endswith("constituentA.drv")
        assert aggregate["constituents"][1].endswith("constituentB.drv")


def test_constituents_glob_misc() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            "--flake",
            ".#legacyPackages.x86_64-linux.glob2",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 6
        assert list(map(lambda x: x["name"], results)) == [
            "constituentA",
            "constituentB",
            "aggregate0",
            "aggregate1",
            "indirect_aggregate0",
            "mix_aggregate0",
        ]
        aggregate = results[2]
        assert len(aggregate["constituents"]) == 2
        assert aggregate["constituents"][0].endswith("constituentA.drv")
        assert aggregate["constituents"][1].endswith("constituentB.drv")
        aggregate = results[4]
        assert len(aggregate["constituents"]) == 1
        assert aggregate["constituents"][0].endswith("aggregate0.drv")
        failed = results[3]
        assert "constituents" in failed
        assert failed["error"] == "tests.*: constituent glob pattern had no matches\n"

        assert results[4]["constituents"][0] == results[2]["drvPath"]
        assert results[5]["constituents"][0] == results[0]["drvPath"]
        assert results[5]["constituents"][1] == results[2]["drvPath"]


def test_constituents_cycle() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            "--flake",
            ".#legacyPackages.x86_64-linux.cycle",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 2
        assert list(map(lambda x: x["name"], results)) == ["aggregate0", "aggregate1"]
        for i in results:
            assert i["error"] == "Dependency cycle: aggregate0 <-> aggregate1"


def test_constituents_error() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            "--flake",
            ".#legacyPackages.x86_64-linux.failures",
            "--constituents",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stdout=subprocess.PIPE,
        )
        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]
        assert len(results) == 2
        child = results[0]
        assert child["attr"] == "doesnteval"
        assert "error" in child
        aggregate = results[1]
        assert aggregate["attr"] == "aggregate"
        assert "namedConstituents" not in aggregate
        assert "doesntexist: does not exist\n" in aggregate["error"]
        assert "constituents" in aggregate


def test_apply() -> None:
    with TemporaryDirectory() as tempdir:
        applyExpr = """drv: {
            the-name = drv.name;
            version = drv.version or null;
        }"""

        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--workers",
            "1",
            "--apply",
            applyExpr,
            "--flake",
            ".#hydraJobs",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            check=True,
            stdout=subprocess.PIPE,
        )

        print(res.stdout)
        results = [json.loads(r) for r in res.stdout.split("\n") if r]

        assert len(results) == 5  # sanity check that we assert against all jobs

        # Check that nix-eval-jobs applied the expression correctly
        # and extracted 'version' as 'version' and 'name' as 'the-name'
        assert results[0]["the-name"] == "job1"
        assert results[0]["version"] is None
        assert results[1]["the-name"].startswith("nix-")
        assert results[1]["version"] is not None
        assert results[2]["the-name"] == "package-with-deps"
        assert results[2]["version"] is None
        assert results[3]["the-name"] == "drvB"
        assert results[3]["version"] is None
        assert results[4]["the-name"].startswith("nix-")
        assert results[4]["version"] is not None


@pytest.mark.infiniterecursion
def test_recursion_error() -> None:
    with TemporaryDirectory() as tempdir:
        cmd = [
            str(BIN),
            "--gc-roots-dir",
            tempdir,
            "--meta",
            "--workers",
            "1",
            "--flake",
            ".#legacyPackages.x86_64-linux.infiniteRecursionPkgs",
        ]
        res = subprocess.run(
            cmd,
            cwd=TEST_ROOT.joinpath("assets"),
            text=True,
            stderr=subprocess.PIPE,
        )
        assert res.returncode == 1
        print(res.stderr)
        assert "packageWithInfiniteRecursion" in res.stderr
        assert "possible infinite recursion" in res.stderr
