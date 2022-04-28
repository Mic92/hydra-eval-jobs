#include <map>
#include <iostream>
#include <thread>

#include <nix/config.h>
#include <nix/shared.hh>
#include <nix/store-api.hh>
#include <nix/eval.hh>
#include <nix/eval-inline.hh>
#include <nix/util.hh>
#include <nix/get-drvs.hh>
#include <nix/globals.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/flake.hh>
#include <nix/attr-path.hh>
#include <nix/derivations.hh>
#include <nix/local-fs-store.hh>
#include <nix/logging.hh>
#include <nix/error.hh>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <nlohmann/json.hpp>

#include "accessor.hh"
#include "args.hh"
#include "drv.hh"
#include "job.hh"
#include "proc.hh"

using namespace nix;

using namespace nix_eval_jobs;

/* `nix-eval-jobs` is meant as an alternative to
   `nix-instantiate`. `nix-instantiate` can use a *lot* of memory
   which is unacceptable in settings where multiple instantiations may
   be happening at the same time. As an example, `nix-eval-jobs` is a
   great program for use in continuous integration (CI). It was
   actually originally extracted from the `hydra` nix CI program.

   `nix-eval-jobs` trades throughput of evaluation for memory by
   forking processes and killing them if they go above a specified
   threshold. This way, the operating system is taking the role of
   garbage collector by simply freeing the whole heap when required.
 */

static Value* releaseExprTopLevelValue(EvalState & state, Bindings & autoArgs) {
    Value vTop;

    state.evalFile(lookupFileArg(state, myArgs.releaseExpr), vTop);

    auto vRoot = state.allocValue();

    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

static Value* flakeTopLevelValue(EvalState & state, Bindings & autoArgs) {
    using namespace flake;

    auto [flakeRef, fragment] = parseFlakeRefWithFragment(myArgs.releaseExpr, absPath("."));

    auto vFlake = state.allocValue();

    auto lockedFlake = lockFlake(state, flakeRef,
        LockFlags {
            .updateLockFile = false,
            .useRegistries = false,
            .allowMutable = false,
        });

    callFlake(state, lockedFlake, *vFlake);

    auto vOutputs = vFlake->attrs->get(state.symbols.create("outputs"))->value;
    state.forceValue(*vOutputs, noPos);
    auto vTop = *vOutputs;

    if (fragment.length() > 0) {
        Bindings & bindings(*state.allocBindings(0));
        auto [nTop, pos] = findAlongAttrPath(state, fragment, bindings, vTop);
        if (!nTop)
            throw Error("error: attribute '%s' missing", nTop);
        vTop = *nTop;
    }

    auto vRoot = state.allocValue();
    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

static Value * topLevelValue(EvalState & state, Bindings & autoArgs) {
    return myArgs.flake
        ? flakeTopLevelValue(state, autoArgs)
        : releaseExprTopLevelValue(state, autoArgs);
}

static void initialAccessorCollector(
    EvalState & state,
    Bindings & autoArgs,
    AutoCloseFD & to,
    AutoCloseFD & from)
{
    auto vRoot = topLevelValue(state, autoArgs);

    nlohmann::json reply;

    if (auto drv = getDerivation(state, *vRoot, false)) {
        reply = Drv(state, *drv, myArgs.meta);

    } else {
    }

    writeLine(to.get(), reply.dump());
}

static void worker(
    EvalState & state,
    Bindings & autoArgs,
    AutoCloseFD & to,
    AutoCloseFD & from)
{
    auto vRoot = topLevelValue(state, autoArgs);

    while (true) {
        /* Wait for the collector to send us a job name. */
        writeLine(to.get(), "next");

        auto s = readLine(from.get());
        if (s == "exit") break;
        if (!hasPrefix(s, "do ")) abort();
        auto pathStr = std::string(s, 3);

        debug("worker process %d at '%s'", getpid(), pathStr);

        nlohmann::json reply;

        /* Evaluate it and send info back to the collector. */
        try {
            auto path = AccessorPath(pathStr);

            reply = nlohmann::json{ { "path", path.toJson() } };

            auto job_ = path.walk(state, autoArgs, *vRoot);

            Job * job;
            if (job_.has_value()) job = job_.value();
            else continue;

            if (auto drvInfo = getDerivation(state, *v, false)) {

                auto drv = Drv(state, *drvInfo);
                reply.update(drv);

                /* Register the derivation as a GC root.  !!! This
                   registers roots for jobs that we may have already
                   done. */
                if (myArgs.gcRootsDir != "") {
                    Path root = myArgs.gcRootsDir + "/" + std::string(baseNameOf(drv.drvPath));
                    if (!pathExists(root)) {
                        auto localStore = state.store.dynamic_pointer_cast<LocalFSStore>();
                        auto storePath = localStore->parseStorePath(drv.drvPath);
                        localStore->addPermRoot(storePath, root);
                    }
                }

            }

            else ;

        } catch (EvalError & e) {
            auto err = e.info();

            std::ostringstream oss;
            showErrorInfo(oss, err, loggerSettings.showTrace.get());
            auto msg = oss.str();

            // Transmits the error we got from the previous evaluation
            // in the JSON output.
            reply["error"] = filterANSIEscapes(msg, true);
            // Don't forget to print it into the STDERR log, this is
            // what's shown in the Hydra UI.
            printError(e.msg());
        }

        writeLine(to.get(), reply.dump());

        /* If our RSS exceeds the maximum, exit. The collector will
           start a new process. */
        struct rusage r;
        getrusage(RUSAGE_SELF, &r);
        if ((size_t) r.ru_maxrss > myArgs.maxMemorySize * 1024) break;
    }

    writeLine(to.get(), "restart");
}

struct State
{
    std::set<nlohmann::json> todo{};
    std::set<nlohmann::json> active;
    std::exception_ptr exc;
};

void collector(Sync<State> & state_, std::condition_variable & wakeup) {
    try {
        std::optional<std::unique_ptr<Proc>> proc_;

        while (true) {

            auto proc = proc_.has_value()
                ? std::move(proc_.value())
                : std::make_unique<Proc>(worker);

            /* Check whether the existing worker process is still there. */
            auto s = readLine(proc->from.get());
            if (s == "restart") {
                proc_ = std::nullopt;
                continue;
            } else if (s != "next") {
                auto json = nlohmann::json::parse(s);
                if (json.find("children") != json.end()) {
                    auto state(state_.lock());
                    for (auto path : json["children"])
                        state->todo.insert(path);

                    continue;
                }
                throw Error("worker error: %s", (std::string) json["error"]);
            }

            /* Wait for a job name to become available. */
            nlohmann::json accessor;

            while (true) {
                checkInterrupt();
                auto state(state_.lock());
                if ((state->todo.empty() && state->active.empty()) || state->exc) {
                    writeLine(proc->to.get(), "exit");
                    return;
                }
                if (!state->todo.empty()) {
                    accessor = *state->todo.begin();
                    state->todo.erase(state->todo.begin());
                    state->active.insert(accessor);
                    break;
                } else
                    state.wait(wakeup);
            }

            /* Tell the worker to evaluate it. */
            writeLine(proc->to.get(), "do " + accessor.dump());

            /* Wait for the response. */
            auto respString = readLine(proc->from.get());
            auto response = nlohmann::json::parse(respString);

            proc_ = std::move(proc);

            /* Print the response. */
            {
                auto state(state_.lock());
                std::cout << response << "\n" << std::flush;
                state->active.erase(accessor);
                wakeup.notify_all();
            }
        }
    } catch (...) {
        auto state(state_.lock());
        state->exc = std::current_exception();
        wakeup.notify_all();
    }
}

void initState(Sync<State> & state_) {
    /* Collect initial attributes to evaluate. This must be done in a
       separate fork to avoid spawning a download in the parent
       process. If that happens, worker processes will try to enqueue
       downloads on their own download threads (which will not
       exist). Then the worker processes will hang forever waiting for
       downloads.
    */
    auto proc = Proc(initialAccessorCollector);

    auto s = readLine(proc.from.get());
    auto json = nlohmann::json::parse(s);

    if (json.find("error") != json.end()) {
        throw Error("getting initial attributes: %s", (std::string) json["error"]);

    } else if (json.find("path") != json.end()) {
        auto state(state_.lock());
        for (auto a : json["path"])
            state->todo.insert(a);

    } else if (json.find("drvPath") != json.end()) {
        std::cout << json.dump() << "\n" << std::flush;

    } else {
        throw Error("expected object with \"error\", \"path\", or a derivation, got: %s", s);

    }
}

int main(int argc, char * * argv)
{
    /* Prevent undeclared dependencies in the evaluation via
       $NIX_PATH. */
    unsetenv("NIX_PATH");

    /* We are doing the garbage collection by killing forks */
    setenv("GC_DONT_GC", "1", 1);

    return handleExceptions(argv[0], [&]() {
        initNix();
        initGC();

        myArgs.parseCmdline(argvToStrings(argc, argv));

        /* FIXME: The build hook in conjunction with import-from-derivation is causing "unexpected EOF" during eval */
        settings.builders = "";

        /* Prevent access to paths outside of the Nix search path and
           to the environment. */
        evalSettings.restrictEval = false;

        /* When building a flake, use pure evaluation (no access to
           'getEnv', 'currentSystem' etc. */
        evalSettings.pureEval = myArgs.evalMode == evalAuto ? myArgs.flake : myArgs.evalMode == evalPure;

        if (myArgs.releaseExpr == "") throw UsageError("no expression specified");

        if (myArgs.gcRootsDir == "") printMsg(lvlError, "warning: `--gc-roots-dir' not specified");

        if (myArgs.showTrace) {
            loggerSettings.showTrace.assign(true);
        }

        Sync<State> state_;
        initState(state_);

        /* Start a collector thread per worker process. */
        std::vector<std::thread> threads;
        std::condition_variable wakeup;
        for (size_t i = 0; i < myArgs.nrWorkers; i++)
            threads.emplace_back(std::thread([&]() { collector(state_, wakeup); }));

        for (auto & thread : threads)
            thread.join();

        auto state(state_.lock());

        if (state->exc)
            std::rethrow_exception(state->exc);

    });
}
