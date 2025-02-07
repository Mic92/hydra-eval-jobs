#include <nix/config.h> // IWYU pragma: keep
// NOLINTBEGIN(modernize-deprecated-headers)
// misc-include-cleaner wants these header rather than the C++ versions
#include <signal.h>
#include <stdlib.h>
#include <string.h>
// NOLINTEND(modernize-deprecated-headers)
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <nix/common-eval-args.hh>
#include <nix/config.hh>
#include <nix/derivations.hh>
#include <nix/error.hh>
#include <nix/eval-gc.hh>
#include <nix/eval-settings.hh>
#include <nix/eval.hh>
#include <nix/file-descriptor.hh>
#include <nix/file-system.hh>
#include <nix/flake/flake.hh>
#include <nix/fmt.hh>
#include <nix/globals.hh>
#include <nix/local-fs-store.hh>
#include <nix/logging.hh>
#include <nix/path.hh>
#include <nix/processes.hh>
#include <nix/shared.hh>
#include <nix/signals.hh>
#include <nix/store-api.hh>
#include <nix/strings.hh>
#include <nix/sync.hh>
#include <nix/terminal.hh>
#include <nlohmann/detail/iterators/iter_impl.hpp>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <pthread.h>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "eval-args.hh"
#include "buffered-io.hh"
#include "worker.hh"
#include "strings-portable.hh"
#include "constituents.hh"

namespace {
MyArgs myArgs; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

using Processor = std::function<void(MyArgs &myArgs, nix::AutoCloseFD &to,
                                     nix::AutoCloseFD &from)>;

/* Auto-cleanup of fork's process and fds. */
struct Proc {
    nix::AutoCloseFD to, from;
    nix::Pid pid;

    Proc(const Proc &) = delete;
    Proc(Proc &&) = delete;
    auto operator=(const Proc &) -> Proc & = delete;
    auto operator=(Proc &&) -> Proc & = delete;

    explicit Proc(const Processor &proc) {
        nix::Pipe toPipe;
        nix::Pipe fromPipe;
        toPipe.create();
        fromPipe.create();
        auto p = startProcess(
            [&,
             to{std::make_shared<nix::AutoCloseFD>(
                 std::move(fromPipe.writeSide))},
             from{std::make_shared<nix::AutoCloseFD>(
                 std::move(toPipe.readSide))}]() {
                nix::logger->log(
                    nix::lvlDebug,
                    nix::fmt("created worker process %d", getpid()));
                try {
                    proc(myArgs, *to, *from);
                } catch (nix::Error &e) {
                    nlohmann::json err;
                    const auto &msg = e.msg();
                    err["error"] = nix::filterANSIEscapes(msg, true);
                    nix::logger->log(nix::lvlError, msg);
                    if (tryWriteLine(to->get(), err.dump()) < 0) {
                        return; // main process died
                    };
                    // Don't forget to print it into the STDERR log, this is
                    // what's shown in the Hydra UI.
                    if (tryWriteLine(to->get(), "restart") < 0) {
                        return; // main process died
                    }
                }
            },
            nix::ProcessOptions{.allowVfork = false});

        to = std::move(toPipe.writeSide);
        from = std::move(fromPipe.readSide);
        pid = p;
    }

    ~Proc() = default;
};

// We'd highly prefer using std::thread here; but this won't let us configure
// the stack size. macOS uses 512KiB size stacks for non-main threads, and musl
// defaults to 128k. While Nix configures a 64MiB size for the main thread, this
// doesn't propagate to the threads we launch here. It turns out, running the
// evaluator under an anemic stack of 0.5MiB has it overflow way too quickly.
// Hence, we have our own custom Thread struct.
struct Thread {
    pthread_t thread = {}; // NOLINT(misc-include-cleaner)

    Thread(const Thread &) = delete;
    Thread(Thread &&) noexcept = default;
    ~Thread() = default;
    auto operator=(const Thread &) -> Thread & = delete;
    auto operator=(Thread &&) -> Thread & = delete;

    explicit Thread(std::function<void(void)> f) {
        pthread_attr_t attr = {}; // NOLINT(misc-include-cleaner)

        auto func = std::make_unique<std::function<void(void)>>(std::move(f));

        int s = pthread_attr_init(&attr);
        if (s != 0) {
            throw nix::SysError(s, "calling pthread_attr_init");
        }
        s = pthread_attr_setstacksize(&attr,
                                      static_cast<size_t>(64) * 1024 * 1024);
        if (s != 0) {
            throw nix::SysError(s, "calling pthread_attr_setstacksize");
        }
        s = pthread_create(&thread, &attr, Thread::init, func.release());
        if (s != 0) {
            throw nix::SysError(s, "calling pthread_launch");
        }
        s = pthread_attr_destroy(&attr);
        if (s != 0) {
            throw nix::SysError(s, "calling pthread_attr_destroy");
        }
    }

    void join() const {
        const int s = pthread_join(thread, nullptr);
        if (s != 0) {
            throw nix::SysError(s, "calling pthread_join");
        }
    }

  private:
    static auto init(void *ptr) -> void * {
        std::unique_ptr<std::function<void(void)>> func;
        func.reset(static_cast<std::function<void(void)> *>(ptr));

        (*func)();
        return nullptr;
    }
};

struct State {
    std::set<nlohmann::json> todo =
        nlohmann::json::array({nlohmann::json::array()});
    std::set<nlohmann::json> active;
    std::map<std::string, nlohmann::json> jobs;
    std::exception_ptr exc;
};

void handleBrokenWorkerPipe(Proc &proc, std::string_view msg) {
    // we already took the process status from Proc, no
    // need to wait for it again to avoid error messages
    const pid_t pid = proc.pid.release();
    while (true) {
        int status = 0;
        const int rc = waitpid(pid, &status, WNOHANG);
        if (rc == 0) {
            kill(pid, SIGKILL);
            throw nix::Error(
                "BUG: while %s, worker pipe got closed but evaluation "
                "worker still running?",
                msg);
        }

        if (rc == -1) {
            kill(pid, SIGKILL);
            throw nix::Error(
                "BUG: while %s, waitpid for evaluation worker failed: %s", msg,
                get_error_name(errno));
        }
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 1) {
                throw nix::Error(
                    "while %s, evaluation worker exited with exit code 1, "
                    "(possible infinite recursion)",
                    msg);
            }
            throw nix::Error("while %s, evaluation worker exited with %d", msg,
                             WEXITSTATUS(status));
        }

        if (WIFSIGNALED(status)) {
            switch (WTERMSIG(status)) {
            case SIGKILL:
                throw nix::Error(
                    "while %s, evaluation worker got killed by SIGKILL, "
                    "maybe "
                    "memory limit reached?",
                    msg);
                break;
#ifdef __APPLE__
            case SIGBUS:
                throw nix::Error(
                    "while %s, evaluation worker got killed by SIGBUS, "
                    "(possible infinite recursion)",
                    msg);
                break;
#else
            case SIGSEGV:
                throw nix::Error(
                    "while %s, evaluation worker got killed by SIGSEGV, "
                    "(possible infinite recursion)",
                    msg);
#endif
            default:
                throw nix::Error("while %s, evaluation worker got killed by "
                                 "signal %d (%s)",
                                 msg, WTERMSIG(status),
                                 get_signal_name(WTERMSIG(status)));
            }
        } // else ignore WIFSTOPPED and WIFCONTINUED
    }
}

auto joinAttrPath(nlohmann::json &attrPath) -> std::string {
    std::string joined;
    for (auto &element : attrPath) {
        if (!joined.empty()) {
            joined += '.';
        }
        joined += element.get<std::string>();
    }
    return joined;
}

void collector(nix::Sync<State> &state_, std::condition_variable &wakeup) {
    try {
        std::optional<std::unique_ptr<Proc>> proc_;
        std::optional<std::unique_ptr<LineReader>> fromReader_;

        while (true) {
            if (!proc_.has_value()) {
                proc_ = std::make_unique<Proc>(worker);
            }
            if (!fromReader_.has_value()) {
                fromReader_ =
                    std::make_unique<LineReader>(proc_.value()->from.release());
            }
            auto proc = std::move(proc_.value());
            auto fromReader = std::move(fromReader_.value());

            /* Check whether the existing worker process is still there. */
            auto s = fromReader->readLine();
            if (s.empty()) {
                handleBrokenWorkerPipe(*proc.get(), "checking worker process");
            } else if (s == "restart") {
                proc_ = std::nullopt;
                fromReader_ = std::nullopt;
                continue;
            } else if (s != "next") {
                try {
                    auto json = nlohmann::json::parse(s);
                    throw nix::Error("worker error: %s",
                                     std::string(json["error"]));
                } catch (const nlohmann::json::exception &e) {
                    throw nix::Error(
                        "Received invalid JSON from worker: %s\n json: '%s'",
                        e.what(), s);
                }
            }

            /* Wait for a job name to become available. */
            nlohmann::json attrPath;

            while (true) {
                nix::checkInterrupt();
                auto state(state_.lock());
                if ((state->todo.empty() && state->active.empty()) ||
                    state->exc) {
                    if (tryWriteLine(proc->to.get(), "exit") < 0) {
                        handleBrokenWorkerPipe(*proc.get(), "sending exit");
                    }
                    return;
                }
                if (!state->todo.empty()) {
                    attrPath = *state->todo.begin();
                    state->todo.erase(state->todo.begin());
                    state->active.insert(attrPath);
                    break;
                }
                state.wait(wakeup);
            }

            /* Tell the worker to evaluate it. */
            if (tryWriteLine(proc->to.get(), "do " + attrPath.dump()) < 0) {
                auto msg = "sending attrPath '" + joinAttrPath(attrPath) + "'";
                handleBrokenWorkerPipe(*proc.get(), msg);
            }

            /* Wait for the response. */
            auto respString = fromReader->readLine();
            if (respString.empty()) {
                auto msg = "reading result for attrPath '" +
                           joinAttrPath(attrPath) + "'";
                handleBrokenWorkerPipe(*proc.get(), msg);
            }
            nlohmann::json response;
            try {
                response = nlohmann::json::parse(respString);
            } catch (const nlohmann::json::exception &e) {
                throw nix::Error(
                    "Received invalid JSON from worker: %s\n json: '%s'",
                    e.what(), respString);
            }

            /* Handle the response. */
            std::vector<nlohmann::json> newAttrs;
            if (response.find("attrs") != response.end()) {
                for (auto &i : response["attrs"]) {
                    nlohmann::json newAttr =
                        nlohmann::json(response["attrPath"]);
                    newAttr.emplace_back(i);
                    newAttrs.push_back(newAttr);
                }
            } else {
                auto state(state_.lock());
                state->jobs.insert_or_assign(response["attr"], response);
                auto named = response.find("namedConstituents");
                if (named == response.end() || named->empty()) {
                    std::cout << respString << "\n" << std::flush;
                }
            }

            proc_ = std::move(proc);
            fromReader_ = std::move(fromReader);

            /* Add newly discovered job names to the queue. */
            {
                auto state(state_.lock());
                state->active.erase(attrPath);
                for (auto &p : newAttrs) {
                    state->todo.insert(p);
                }
                wakeup.notify_all();
            }
        }
    } catch (...) {
        auto state(state_.lock());
        state->exc = std::current_exception();
        wakeup.notify_all();
    }
}
} // namespace

auto main(int argc, char **argv) -> int {

    /* Prevent undeclared dependencies in the evaluation via
       $NIX_PATH. */
    unsetenv("NIX_PATH"); // NOLINT(concurrency-mt-unsafe)

    /* We are doing the garbage collection by killing forks */
    setenv("GC_DONT_GC", "1", 1); // NOLINT(concurrency-mt-unsafe)

    /* Because of an objc quirk[1], calling curl_global_init for the first time
       after fork() will always result in a crash.
       Up until now the solution has been to set
       OBJC_DISABLE_INITIALIZE_FORK_SAFETY for every nix process to ignore that
       error. Instead of working around that error we address it at the core -
       by calling curl_global_init here, which should mean curl will already
       have been initialized by the time we try to do so in a forked process.

       [1]
       https://github.com/apple-oss-distributions/objc4/blob/01edf1705fbc3ff78a423cd21e03dfc21eb4d780/runtime/objc-initialize.mm#L614-L636
    */
    curl_global_init(CURL_GLOBAL_ALL);

    auto args = std::span(argv, argc);

    return nix::handleExceptions(args[0], [&]() {
        nix::initNix();
        nix::initGC();
        nix::flake::initLib(nix::flakeSettings);

        std::optional<nix::AutoDelete> gcRootsDir = std::nullopt;

        myArgs.parseArgs(argv, argc);

        /* FIXME: The build hook in conjunction with import-from-derivation is
         * causing "unexpected EOF" during eval */
        nix::settings.builders = "";

        /* Prevent access to paths outside of the Nix search path and
           to the environment. */
        nix::evalSettings.restrictEval = false;

        /* When building a flake, use pure evaluation (no access to
           'getEnv', 'currentSystem' etc. */
        if (myArgs.impure) {
            nix::evalSettings.pureEval = false;
        } else if (myArgs.flake) {
            nix::evalSettings.pureEval = true;
        }

        if (myArgs.releaseExpr.empty()) {
            throw nix::UsageError("no expression specified");
        }

        if (myArgs.gcRootsDir.empty()) {
            const nix::Path tmpDir = nix::createTempDir();
            gcRootsDir.emplace(tmpDir, true);
            myArgs.gcRootsDir = tmpDir;
        } else {
            myArgs.gcRootsDir = std::filesystem::absolute(myArgs.gcRootsDir);
        }

        if (myArgs.showTrace) {
            nix::loggerSettings.showTrace.assign(true);
        }

        nix::Sync<State> state_;

        /* Start a collector thread per worker process. */
        std::vector<Thread> threads;
        std::condition_variable wakeup;
        threads.reserve(myArgs.nrWorkers);
        for (size_t i = 0; i < myArgs.nrWorkers; i++) {
            threads.emplace_back(
                [&state_, &wakeup] { collector(state_, wakeup); });
        }

        for (auto &thread : threads) {
            thread.join();
        }

        auto state(state_.lock());

        if (state->exc) {
            std::rethrow_exception(state->exc);
        }

        if (myArgs.constituents) {
            auto store = myArgs.evalStoreUrl
                             ? nix::openStore(*myArgs.evalStoreUrl)
                             : nix::openStore();

            std::visit(
                nix::overloaded{
                    [&](const std::vector<AggregateJob> &namedConstituents) {
                        rewriteAggregates(state->jobs, namedConstituents, store,
                                          myArgs.gcRootsDir);
                    },
                    [&](const DependencyCycle &e) {
                        nix::logger->log(nix::lvlError,
                                         nix::fmt("Found dependency cycle "
                                                  "between jobs '%s' and '%s'",
                                                  e.a, e.b));
                        state->jobs[e.a]["error"] = e.message();
                        state->jobs[e.b]["error"] = e.message();

                        std::cout << state->jobs[e.a].dump() << "\n"
                                  << state->jobs[e.b].dump() << "\n";

                        for (const auto &jobName : e.remainingAggregates) {
                            state->jobs[jobName]["error"] =
                                "Skipping aggregate because of a dependency "
                                "cycle";
                            std::cout << state->jobs[jobName].dump() << "\n";
                        }
                    },
                },
                resolveNamedConstituents(state->jobs));
        }
    });
}
