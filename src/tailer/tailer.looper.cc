/**
 * Copyright (c) 2021, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <regex>
#include <string>

#include "tailer.looper.hh"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "base/auto_fd.hh"
#include "base/auto_pid.hh"
#include "base/file_range.hh"
#include "base/fs_util.hh"
#include "base/humanize.hh"
#include "base/humanize.network.hh"
#include "base/lnav_log.hh"
#include "base/paths.hh"
#include "base/result.h"
#include "base/string_util.hh"
#include "config.h"
#include "fmt/chrono.h"
#include "fmt/ranges.h"
#include "line_buffer.hh"
#include "lnav.hh"
#include "lnav.indexing.hh"
#include "lnav.prompt.hh"
#include "service_tags.hh"
#include "tailer.h"
#include "tailer.looper.cfg.hh"
#include "tailerbin.h"
#include "tailerpp.hh"

using namespace std::chrono_literals;

static const auto HOST_RETRY_DELAY = 1min;

using msg_level = lnav::console::user_message::level;

/**
 * Apply a change to the status of a remote and let the main thread know so
 * that the details panel can be refreshed.
 */
static void
notify_remote_status_changed(const std::string& netloc)
{
    isc::to<main_looper&, services::main_t>().send([netloc](auto& mlooper) {
        lnav_data.ld_files_source.remote_status_changed(netloc);
    });
}

template<typename F>
static void
update_remote_status(const std::string& netloc, F func)
{
    {
        auto sp = lnav_data.ld_active_files.fc_progress->writeAccess();

        func(sp->sp_remotes[netloc]);
    }
    notify_remote_status_changed(netloc);
}

static void
remote_log(const std::string& netloc, lnav::console::user_message um)
{
    update_remote_status(netloc,
                         [&](remote_status& rs) { rs.add_log(std::move(um)); });
}

static void
set_remote_state(const std::string& netloc,
                 msg_level level,
                 std::string state,
                 std::optional<lnav::console::user_message> log_msg
                 = std::nullopt)
{
    update_remote_status(netloc, [&](remote_status& rs) {
        rs.rs_state = std::move(state);
        rs.rs_state_level = level;
        if (log_msg) {
            rs.add_log(std::move(log_msg.value()));
        }
    });
}

static void
read_err_pipe(const std::string& netloc,
              auto_fd& err,
              tailer::safe_error_queue& eq)
{
    line_buffer lb;
    file_range pipe_range;
    bool done = false;

    log_info("stderr reader started...");
    lb.set_fd(err);
    while (!done) {
        auto load_res = lb.load_next_line(pipe_range);

        if (load_res.isErr()) {
            done = true;
        } else {
            auto li = load_res.unwrap();

            pipe_range = li.li_file_range;
            if (li.li_file_range.empty()) {
                done = true;
            } else {
                lb.read_range(li.li_file_range).then([netloc, &eq](auto sbr) {
                    auto line_str
                        = string_fragment(sbr.get_data(), 0, sbr.length())
                              .trim("\n");
                    {
                        safe::WriteAccess<tailer::safe_error_queue> writable_eq(
                            eq);

                        // The last lines are the ones closest to a
                        // failure, so the oldest are dropped.
                        if (writable_eq->size() >= 10) {
                            writable_eq->erase(writable_eq->begin());
                        }
                        writable_eq->emplace_back(line_str.to_string());
                    }

                    auto level = line_str.startswith("error:")
                        ? lnav_log_level_t::ERROR
                        : line_str.startswith("warning:")
                        ? lnav_log_level_t::WARNING
                        : line_str.startswith("info:")
                        ? lnav_log_level_t::INFO
                        : lnav_log_level_t::DEBUG;
                    log_msg_wrapper(level,
                                    "tailer[%s] %.*s",
                                    netloc.c_str(),
                                    line_str.length(),
                                    line_str.data());
                    // Before the tailer starts, everything on stderr comes
                    // from the transport or the bootstrap script and could
                    // explain a failure.  After that, the tailer reports
                    // every block it offers, which would push the
                    // interesting lines out, so only problems are kept.
                    {
                        auto sp = lnav_data.ld_active_files.fc_progress
                                      ->writeAccess();
                        auto& rs = sp->sp_remotes[netloc];

                        if (rs.rs_connected && level != lnav_log_level_t::ERROR
                            && level != lnav_log_level_t::WARNING)
                        {
                            return;
                        }
                        auto al = attr_line_t("remote: ")
                                      .append(attr_line_t::from_ansi_str(
                                          line_str.to_string()));
                        rs.add_log(
                            level == lnav_log_level_t::ERROR
                                ? lnav::console::user_message::error(al)
                                : level == lnav_log_level_t::WARNING
                                ? lnav::console::user_message::warning(al)
                                : lnav::console::user_message::raw(al));
                    }
                    notify_remote_status_changed(netloc);
                });
            }
        }
    }
}

static void
update_tailer_progress(const std::string& netloc, const std::string& msg)
{
    lnav_data.ld_active_files.fc_progress->writeAccess()
        ->sp_tailers[netloc]
        .tp_message = msg;
    set_remote_state(
        netloc, msg_level::info, msg, lnav::console::user_message::raw(msg));
}

static void
update_tailer_description(
    const std::string& netloc,
    const std::map<std::string, logfile_open_options_base>& desired_paths,
    const std::string& remote_uname)
{
    std::vector<std::string> paths;

    for (const auto& des_pair : desired_paths) {
        paths.emplace_back(
            fmt::format(FMT_STRING("{}{}"), netloc, des_pair.first));
    }
    isc::to<main_looper&, services::main_t>().send(
        [netloc, paths, remote_uname](auto& mlooper) {
            auto& fc = lnav_data.ld_active_files;

            for (const auto& path : paths) {
                auto iter = fc.fc_other_files.find(path);

                if (iter == fc.fc_other_files.end()) {
                    continue;
                }

                iter->second.ofd_description = remote_uname;
            }
            fc.fc_name_to_stubs->writeAccess()->erase(netloc);
        });
}

void
tailer::looper::loop_body()
{
    service_base::loop_body();

    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> to_erase;

    for (auto& qpair : this->l_netlocs_to_paths) {
        auto& netloc = qpair.first;
        auto& rpq = qpair.second;

        if (now < rpq.rpq_next_attempt_time) {
            continue;
        }
        if (this->l_remotes.count(netloc) == 0) {
            auto create_res = host_tailer::for_host(netloc);

            if (create_res.isErr()) {
                set_remote_state(
                    netloc,
                    msg_level::error,
                    "Unable to connect",
                    lnav::console::user_message::error(create_res.unwrapErr()));
                report_error(netloc, create_res.unwrapErr());
                if (rpq.has_unfollowed_paths()) {
                    rpq.send_synced_to_main(netloc);
                    to_erase.push_back(netloc);
                } else {
                    rpq.rpq_next_attempt_time = now + HOST_RETRY_DELAY;
                    remote_log(
                        netloc,
                        lnav::console::user_message::raw(fmt::format(
                            FMT_STRING("retrying in {}"), HOST_RETRY_DELAY)));
                }
                continue;
            }

            auto ht = create_res.unwrap();
            this->l_remotes[netloc] = ht;
            this->s_children.add_child_service(ht);

            rpq.rpq_new_paths.insert(rpq.rpq_existing_paths.begin(),
                                     rpq.rpq_existing_paths.end());
            rpq.rpq_existing_paths.clear();
        }

        if (!rpq.rpq_new_paths.empty()) {
            log_debug("%s: new paths to monitor -- %s",
                      netloc.c_str(),
                      rpq.rpq_new_paths.begin()->first.c_str());
            this->l_remotes[netloc]->send(
                [paths = rpq.rpq_new_paths](auto& ht) {
                    for (const auto& pair : paths) {
                        log_debug("adding path to tailer -- %s",
                                  pair.first.c_str());
                        ht.open_remote_path(pair.first, std::move(pair.second));
                    }
                });

            rpq.rpq_existing_paths.insert(rpq.rpq_new_paths.begin(),
                                          rpq.rpq_new_paths.end());
            rpq.rpq_new_paths.clear();
        }
    }

    for (const auto& netloc : to_erase) {
        this->l_netlocs_to_paths.erase(netloc);
    }
}

void
tailer::looper::add_remote(const network::path& path,
                           logfile_open_options_base options)
{
    auto netloc_str = fmt::to_string(path.home());
    this->l_netlocs_to_paths[netloc_str].rpq_new_paths[path.p_path]
        = std::move(options);
}

void
tailer::looper::load_preview(int64_t id, const network::path& path)
{
    auto netloc_str = fmt::to_string(path.home());
    auto iter = this->l_remotes.find(netloc_str);

    if (iter == this->l_remotes.end()) {
        auto create_res = host_tailer::for_host(netloc_str);

        if (create_res.isErr()) {
            auto msg = create_res.unwrapErr();
            isc::to<main_looper&, services::main_t>().send(
                [id, msg](auto& mlooper) {
                    if (lnav_data.ld_preview_generation != id) {
                        return;
                    }
                    lnav_data.ld_preview_status_source[0]
                        .get_description()
                        .set_cylon(false)
                        .clear();
                    lnav_data.ld_preview_source[0].clear();
                    lnav_data.ld_bottom_source.grep_error(msg);
                    lnav_data.ld_status[LNS_BOTTOM].set_needs_update();
                });
            return;
        }

        auto ht = create_res.unwrap();
        this->l_remotes[netloc_str] = ht;
        this->s_children.add_child_service(ht);
    }

    this->l_remotes[netloc_str]->send([id, file_path = path.p_path](auto& ht) {
        ht.load_preview(id, file_path);
    });
}

void
tailer::looper::complete_path(const network::path& path)
{
    auto netloc_str = fmt::to_string(path.home());
    auto iter = this->l_remotes.find(netloc_str);

    if (iter == this->l_remotes.end()) {
        auto create_res = host_tailer::for_host(netloc_str);

        if (create_res.isErr()) {
            return;
        }

        auto ht = create_res.unwrap();
        this->l_remotes[netloc_str] = ht;
        this->s_children.add_child_service(ht);
    }

    this->l_remotes[netloc_str]->send(
        [file_path = path.p_path](auto& ht) { ht.complete_path(file_path); });
}

static std::vector<std::string>
create_ssh_args(const tailer::config& cfg,
                const network::locality& loc,
                const std::string& remote_cmd)
{
    std::vector<std::string> retval;

    retval.emplace_back(cfg.c_ssh_cmd);
    if (!cfg.c_ssh_flags.empty()) {
        if (startswith(cfg.c_ssh_flags, "-")) {
            retval.emplace_back(cfg.c_ssh_flags);
        } else {
            retval.emplace_back(
                fmt::format(FMT_STRING("-{}"), cfg.c_ssh_flags));
        }
    }
    for (const auto& pair : cfg.c_ssh_options) {
        if (pair.second.empty()) {
            continue;
        }
        retval.emplace_back(fmt::format(FMT_STRING("-{}"), pair.first));
        retval.emplace_back(pair.second);
    }
    for (const auto& pair : cfg.c_ssh_config) {
        if (pair.second.empty()) {
            continue;
        }
        retval.emplace_back(
            fmt::format(FMT_STRING("-o{}={}"), pair.first, pair.second));
    }
    if (loc.l_username) {
        retval.emplace_back(fmt::format(
            FMT_STRING("{}@{}"), loc.l_username.value(), loc.l_hostname));
    } else {
        retval.emplace_back(loc.l_hostname);
    }

    // The remote login shell might not be POSIX-compatible, so run the
    // command through sh.
    std::string quoted_cmd = "sh -c '";
    for (const auto ch : remote_cmd) {
        if (ch == '\'') {
            quoted_cmd.append("'\\''");
        } else {
            quoted_cmd.push_back(ch);
        }
    }
    quoted_cmd.push_back('\'');
    retval.emplace_back(quoted_cmd);

    return retval;
}

static std::vector<std::string>
create_kubectl_args(const tailer::config& cfg,
                    const network::locality& loc,
                    const std::string& remote_cmd)
{
    std::vector<std::string> retval;

    retval.emplace_back(cfg.c_kubectl_cmd);
    split_ws(cfg.c_kubectl_flags, retval);
    retval.emplace_back("-n");
    retval.emplace_back(loc.l_namespace.value_or("default"));
    retval.emplace_back("exec");
    retval.emplace_back("-i");
    retval.emplace_back("-q");
    retval.emplace_back(loc.l_hostname);
    if (loc.l_container) {
        retval.emplace_back("-c");
        retval.emplace_back(loc.l_container.value());
    }
    retval.emplace_back("--");
    retval.emplace_back("sh");
    retval.emplace_back("-c");
    retval.emplace_back(remote_cmd);

    return retval;
}

static std::vector<std::string>
create_transport_args(const network::locality& loc,
                      const std::string& remote_cmd)
{
    const auto& cfg = injector::get<const tailer::config&>();

    switch (loc.l_transport) {
        case network::transport_t::kubectl:
            return create_kubectl_args(cfg, loc, remote_cmd);
        case network::transport_t::ssh:
            break;
    }
    return create_ssh_args(cfg, loc, remote_cmd);
}

/**
 * Prefix for the bootstrap script in a container, where the working
 * directory might not be writable or might be on a "noexec" mount.  An
 * empty file is used to check if programs can be run, the shell runs it as
 * an empty script if the exec succeeds.
 */
static constexpr auto CONTAINER_WORK_DIR_SCRIPT
    = R"(w=; t=.lnav-exec-check.$$; )"
      R"(for d in $TMPDIR /tmp /var/tmp $HOME; do )"
      R"(cd "$d" 2>/dev/null || continue; )"
      R"(true 2>/dev/null > "$t" && chmod +x "$t" && ./"$t" 2>/dev/null )"
      R"(&& w=1; rm -f "$t"; [ -n "$w" ] && break; done; )"
      R"([ -n "$w" ] || { echo "error: no writable directory that allows )"
      R"(running programs was found" >&2; exit 1; }; )";

/**
 * The line printed by the bootstrap script to tell lnav which tailer
 * implementation should be sent.
 */
static constexpr auto BOOTSTRAP_IMPL_MARKER = "lnav-tailer-impl: "_frag;

/**
 * Build the script that runs on the remote side to receive and start the
 * tailer over a single connection.  The script prints which implementation
 * it wants, reads the size of the implementation as a line on stdin, reads
 * that many bytes into a file, and then starts the tailer with the same
 * stdin/stdout so the connection becomes the protocol channel.
 */
static std::string
create_bootstrap_script(bool in_container,
                        const std::string& tailer_bin_name,
                        const std::string& start_cmd)
{
    return fmt::format(
        FMT_STRING("{}"
                   "if command -v python3 >/dev/null 2>&1; "
                   "then i=py; else i=ape; fi; "
                   "echo \"{}$i\"; "
                   "read -r n || exit 1; "
                   "head -c \"$n\" > {} && chmod ugo+rx ./{} && {}"),
        in_container ? CONTAINER_WORK_DIR_SCRIPT : "",
        BOOTSTRAP_IMPL_MARKER,
        tailer_bin_name,
        tailer_bin_name,
        fmt::format(fmt::runtime(start_cmd), tailer_bin_name));
}

Result<std::shared_ptr<tailer::looper::host_tailer>, std::string>
tailer::looper::host_tailer::for_host(const std::string& netloc)
{
    log_debug("tailer(%s): transferring tailer to remote", netloc.c_str());

    update_tailer_progress(netloc, "Transferring tailer...");

    auto& cfg = injector::get<const tailer::config&>();
    auto tailer_bin_name = fmt::format(FMT_STRING("tailer.bin.{}"), getpid());

    auto rp = humanize::network::path::from_str(netloc).value();
    const auto is_kubectl
        = rp.p_locality.l_transport == network::transport_t::kubectl;
    const auto script = create_bootstrap_script(
        is_kubectl,
        tailer_bin_name,
        is_kubectl ? cfg.c_kubectl_start_cmd : cfg.c_start_cmd);

    // Logged here instead of in the child so the child's stderr only has
    // output from the remote side, which is used for error messages.
    auto arg_strs = create_transport_args(rp.p_locality, script);
    log_info("tailer(%s): executing -- %s",
             netloc.c_str(),
             fmt::to_string(fmt::join(arg_strs, " ")).c_str());
    // The script is long and not interesting to the user, so only the
    // command that runs it is shown.
    remote_log(
        netloc,
        lnav::console::user_message::raw(fmt::format(
            FMT_STRING("executing: {} ..."),
            fmt::join(arg_strs.begin(), std::prev(arg_strs.end()), " "))));

    auto in_pipe = TRY(auto_pipe::for_child_fd(STDIN_FILENO));
    auto out_pipe = TRY(auto_pipe::for_child_fd(STDOUT_FILENO));
    auto err_pipe = TRY(auto_pipe::for_child_fd(STDERR_FILENO));
    auto child = TRY(lnav::pid::from_fork());

    in_pipe.after_fork(child.in());
    out_pipe.after_fork(child.in());
    err_pipe.after_fork(child.in());

    if (child.in_child()) {
        std::vector<char*> args;

        for (const auto& arg : arg_strs) {
            args.push_back((char*) arg.data());
        }
        args.push_back(nullptr);

        execvp(args[0], args.data());
        _exit(EXIT_FAILURE);
    }

    auto retval = std::make_shared<host_tailer>(netloc,
                                                std::move(child),
                                                std::move(in_pipe.write_end()),
                                                std::move(out_pipe.read_end()),
                                                std::move(err_pipe.read_end()));
    auto bootstrap_res = retval->bootstrap();
    if (bootstrap_res.isErr()) {
        auto error_msg = bootstrap_res.unwrapErr();

        log_warning("tailer bootstrap failed: %s", error_msg.c_str());
        return Err(
            is_kubectl
                ? fmt::format(FMT_STRING("failed to exec in container: {}"),
                              error_msg)
                : fmt::format(FMT_STRING("failed to ssh to host: {}"),
                              error_msg));
    }

    return Ok(retval);
}

/**
 * How long to wait for the remote side to make progress while the tailer
 * is being transferred and started.
 */
static constexpr auto BOOTSTRAP_TIMEOUT = 30s;

/**
 * Wait for the given events on the FD.
 *
 * @return false if the timeout expired.
 */
static bool
wait_for_fd(int fd, short events)
{
    static constexpr auto TIMEOUT_MS
        = std::chrono::duration_cast<std::chrono::milliseconds>(
              BOOTSTRAP_TIMEOUT)
              .count();

    while (true) {
        pollfd pfd{fd, events, 0};
        auto rc = poll(&pfd, 1, TIMEOUT_MS);

        if (rc < 0 && errno == EINTR) {
            continue;
        }
        // Errors are reported by the read()/write() that follows.
        return rc != 0;
    }
}

Result<void, std::string>
tailer::looper::host_tailer::bootstrap()
{
    auto& conn = this->ht_state.get<connected>();

    auto fail = [this, &conn](std::string msg) -> Result<void, std::string> {
        auto finished_child = std::move(conn).close();

        this->ht_state = disconnected();
        if (this->ht_error_reader.joinable()) {
            this->ht_error_reader.join();
        }

        // The remote's stderr usually says why, e.g. an authentication
        // failure behind a closed connection, but isn't a substitute for
        // what went wrong here.
        auto readable_eq = this->ht_error_queue.readAccess();
        if (!readable_eq->empty()) {
            msg = fmt::format(FMT_STRING("{} -- {}"), msg, readable_eq->back());
        }
        return Err(msg);
    };
    auto timed_out = [&conn, &fail](const char* what) {
        // The child might be stuck, so it would not exit when its stdin is
        // closed.
        kill(conn.ht_child.in(), SIGTERM);
        return fail(fmt::format(FMT_STRING("timed out after {} seconds {}"),
                                BOOTSTRAP_TIMEOUT.count(),
                                what));
    };
    auto write_bits = [&conn](string_fragment sf) -> Result<bool, std::string> {
        // Writes of at most PIPE_BUF bytes do not block once poll() says the
        // pipe is writable.
        while (!sf.empty()) {
            if (!wait_for_fd(conn.ht_to_child.get(), POLLOUT)) {
                return Ok(false);
            }
            auto chunk = sf.sub_range(
                0, std::min(sf.length(), static_cast<int>(PIPE_BUF)));
            TRY(conn.ht_to_child.write_fully(chunk));
            sf = sf.substr(chunk.length());
        }
        return Ok(true);
    };

    std::optional<std::string> impl_name;
    {
        std::string line;

        while (!impl_name) {
            if (!wait_for_fd(conn.ht_from_child.get(), POLLIN)) {
                return timed_out("waiting for the remote command to start");
            }

            char ch;
            auto rc = read(conn.ht_from_child.get(), &ch, 1);

            if (rc < 0 && errno == EINTR) {
                continue;
            }
            if (rc <= 0) {
                return fail("connection closed before the tailer was sent");
            }
            if (ch != '\n') {
                if (line.size() > 4096) {
                    return fail("unexpected output from the remote command");
                }
                line.push_back(ch);
                continue;
            }

            auto line_sf = string_fragment::from_str(line);
            if (line_sf.startswith(BOOTSTRAP_IMPL_MARKER.data())) {
                impl_name = line_sf.substr(BOOTSTRAP_IMPL_MARKER.length())
                                .to_string();
            } else {
                log_debug("bootstrap output -- %s", line.c_str());
                remote_log(this->ht_netloc,
                           lnav::console::user_message::raw(
                               attr_line_t("remote: ")
                                   .append(attr_line_t::from_ansi_str(line))));
            }
            line.clear();
        }
    }

    const auto suffix = fmt::format(FMT_STRING(".{}"), impl_name.value());
    const bin_src_file* tailer_impl = nullptr;
    for (const auto& impl : tailer_bin) {
        if (impl.get_name().endswith(suffix.c_str())) {
            tailer_impl = &impl;
            break;
        }
    }
    if (tailer_impl == nullptr) {
        return fail(fmt::format(FMT_STRING("unknown tailer implementation: {}"),
                                impl_name.value()));
    }

    log_info("sending tailer implementation: %s",
             tailer_impl->get_name().to_string().c_str());
    remote_log(this->ht_netloc,
               lnav::console::user_message::raw(fmt::format(
                   FMT_STRING("sending the {} tailer ({})"),
                   impl_name.value() == "py" ? "Python" : "native",
                   humanize::file_size(tailer_impl->get_uncompressed_size(),
                                       humanize::alignment::none))));
    auto size_line
        = fmt::format(FMT_STRING("{}\n"), tailer_impl->get_uncompressed_size());
    auto write_res = write_bits(string_fragment::from_str(size_line));
    if (write_res.isErr()) {
        return fail(write_res.unwrapErr());
    }
    if (!write_res.unwrap()) {
        return timed_out("sending the tailer");
    }
    auto sfp = tailer_impl->to_string_fragment_producer();
    while (true) {
        auto next_res = sfp->next();
        if (next_res.is<string_fragment_producer::eof>()) {
            break;
        }
        if (next_res.is<string_fragment_producer::error>()) {
            return fail(next_res.get<string_fragment_producer::error>().what);
        }

        auto block_write_res = write_bits(next_res.get<string_fragment>());
        if (block_write_res.isErr()) {
            return fail(block_write_res.unwrapErr());
        }
        if (!block_write_res.unwrap()) {
            return timed_out("sending the tailer");
        }
    }

    update_tailer_progress(this->ht_netloc, "Starting tailer...");

    // Wait for the tailer to announce itself so nothing else is written to
    // the connection while the bootstrap script is still reading the tailer.
    if (!wait_for_fd(conn.ht_from_child.get(), POLLIN)) {
        return timed_out("waiting for the tailer to start");
    }
    auto packet_res = tailer::read_packet(conn.ht_from_child.get());
    if (packet_res.isErr()) {
        return fail(packet_res.unwrapErr());
    }
    auto packet = packet_res.unwrap();
    if (!packet.is<tailer::packet_announce>()) {
        return fail("the tailer did not start");
    }
    this->ht_uname = packet.get<tailer::packet_announce>().pa_uname;
    update_tailer_description(
        this->ht_netloc, conn.c_desired_paths, this->ht_uname);
    update_remote_status(this->ht_netloc, [this](remote_status& rs) {
        rs.rs_state = "Syncing";
        rs.rs_state_level = msg_level::info;
        rs.rs_connected = true;
        rs.rs_uname = this->ht_uname;
        rs.add_log(lnav::console::user_message::ok(
            fmt::format(FMT_STRING("tailer started on: {}"), this->ht_uname)));
    });

    return Ok();
}

static std::filesystem::path
remote_cache_path()
{
    return lnav::paths::workdir() / "remotes";
}

std::filesystem::path
tailer::looper::host_tailer::tmp_path()
{
    auto local_path = remote_cache_path();

    std::filesystem::create_directories(local_path);
    auto_mem<char> resolved_path;

    resolved_path = realpath(local_path.c_str(), nullptr);
    if (resolved_path.in() == nullptr) {
        return local_path;
    }

    return resolved_path.in();
}

static std::string
scrub_netloc(const std::string& netloc)
{
    const static std::regex TO_SCRUB(R"([^\w\.\@])");

    return std::regex_replace(netloc, TO_SCRUB, "_");
}

tailer::looper::host_tailer::host_tailer(const std::string& netloc,
                                         auto_pid<process_state::running> child,
                                         auto_fd to_child,
                                         auto_fd from_child,
                                         auto_fd err_from_child)
    : isc::service<host_tailer>(netloc), ht_netloc(netloc),
      ht_local_path(tmp_path() / scrub_netloc(netloc)),
      ht_error_reader([netloc,
                       err = std::move(err_from_child),
                       &eq = this->ht_error_queue]() mutable {
          read_err_pipe(netloc, err, eq);
      }),
      ht_state(connected{
          std::move(child), std::move(to_child), std::move(from_child), {}})
{
}

void
tailer::looper::host_tailer::open_remote_path(const std::string& path,
                                              logfile_open_options_base loo)
{
    this->ht_state.match(
        [&](connected& conn) {
            remote_log(
                this->ht_netloc,
                lnav::console::user_message::raw(
                    attr_line_t("opening: ").append(lnav::roles::file(path))));
            conn.c_desired_paths[path] = std::move(loo);
            send_packet(conn.ht_to_child.get(),
                        TPT_OPEN_PATH,
                        TPPT_STRING,
                        path.c_str(),
                        TPPT_DONE);
        },
        [&](const disconnected& d) {
            log_warning("disconnected from host, cannot tail: %s",
                        path.c_str());
        },
        [&](const synced& s) {
            log_warning("synced with host, not tailing: %s", path.c_str());
        });
}

void
tailer::looper::host_tailer::report_preview_disconnect(int64_t id) const
{
    log_warning("no connection to host, cannot preview: %s",
                this->ht_netloc.c_str());

    auto msg = fmt::format(FMT_STRING("error: disconnected from {}"),
                           this->ht_netloc);
    isc::to<main_looper&, services::main_t>().send([id, msg](auto& mlooper) {
        if (lnav_data.ld_preview_generation != id) {
            return;
        }
        lnav_data.ld_preview_status_source[0]
            .get_description()
            .set_cylon(false)
            .set_value(msg);
        lnav_data.ld_status[LNS_PREVIEW0].set_needs_update();
    });
}

void
tailer::looper::host_tailer::load_preview(int64_t id, const std::string& path)
{
    this->ht_state.match(
        [&](connected& conn) {
            send_packet(conn.ht_to_child.get(),
                        TPT_LOAD_PREVIEW,
                        TPPT_STRING,
                        path.c_str(),
                        TPPT_INT64,
                        id,
                        TPPT_DONE);
        },
        [&](const disconnected& d) { this->report_preview_disconnect(id); },
        [&](const synced& s) { this->report_preview_disconnect(id); });
}

void
tailer::looper::host_tailer::complete_path(const std::string& path)
{
    this->ht_state.match(
        [&](connected& conn) {
            send_packet(conn.ht_to_child.get(),
                        TPT_COMPLETE_PATH,
                        TPPT_STRING,
                        path.c_str(),
                        TPPT_DONE);
        },
        [&](const disconnected& d) {
            log_warning("disconnected from host, cannot complete: %s",
                        path.c_str());
        },
        [&](const synced& s) {
            log_warning("synced with host, cannot complete: %s", path.c_str());
        });
}

void
tailer::looper::host_tailer::loop_body()
{
    const static uint64_t TOUCH_FREQ = 10000;

    if (!this->ht_state.is<connected>()) {
        return;
    }

    this->ht_cycle_count += 1;
    if (this->ht_cycle_count % TOUCH_FREQ == 0) {
        auto now = std::filesystem::file_time_type::clock::now();
        std::error_code ec;

        // The local path is only created once a file has been transferred,
        // so this can legitimately fail.
        std::filesystem::last_write_time(this->ht_local_path, now, ec);
    }

    auto& conn = this->ht_state.get<connected>();

    pollfd pfds[1];

    pfds[0].fd = conn.ht_from_child.get();
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;

    auto ready_count = poll(pfds, 1, 100);
    if (ready_count > 0) {
        auto read_res = tailer::read_packet(conn.ht_from_child);

        if (read_res.isErr()) {
            auto errmsg = read_res.unwrapErr();

            // The stream is out of sync at this point, there is no way to
            // recover, so shut the connection down.
            log_error("tailer(%s): read error: %s",
                      this->ht_netloc.c_str(),
                      errmsg.c_str());
            set_remote_state(this->ht_netloc,
                             msg_level::error,
                             "Disconnected",
                             lnav::console::user_message::error("read error")
                                 .with_reason(errmsg));
            report_error(this->ht_netloc, errmsg);
            auto finished_child = std::move(conn).close();
            log_debug("tailer(%s): exit status %d",
                      this->ht_netloc.c_str(),
                      finished_child.exit_status());
            this->ht_state = disconnected();
            this->s_looping = false;
            return;
        }

        auto packet = read_res.unwrap();
        this->ht_state = packet.match(
            [&](const tailer::packet_eof& te) {
                log_debug("all done!");

                auto finished_child = std::move(conn).close();
                auto readable_eq = this->ht_error_queue.readAccess();
                if (finished_child.exit_status() != 0
                    && !readable_eq->empty()) {
                    report_error(this->ht_netloc, readable_eq->back());
                }
                auto closed_um = lnav::console::user_message::error(
                    fmt::format(FMT_STRING("connection closed, exit status {}"),
                                finished_child.exit_status()));
                if (finished_child.exit_status() == 0) {
                    closed_um.um_level = msg_level::warning;
                }
                set_remote_state(this->ht_netloc,
                                 closed_um.um_level,
                                 "Disconnected",
                                 closed_um);

                return state_v{disconnected()};
            },
            [&](const tailer::packet_announce& pa) {
                update_tailer_description(
                    this->ht_netloc, conn.c_desired_paths, pa.pa_uname);
                this->ht_uname = pa.pa_uname;
                return std::move(this->ht_state);
            },
            [&](const tailer::packet_log& pl) {
                log_debug("%s\n", pl.pl_msg.c_str());
                return std::move(this->ht_state);
            },
            [&](const tailer::packet_error& pe) {
                log_debug("Got an error: %s -- %s",
                          pe.pe_path.c_str(),
                          pe.pe_msg.c_str());
                remote_log(
                    this->ht_netloc,
                    lnav::console::user_message::error(
                        attr_line_t().append(lnav::roles::file(pe.pe_path)))
                        .with_reason(pe.pe_msg));

                lnav_data.ld_active_files.fc_progress->writeAccess()
                    ->sp_tailers.erase(this->ht_netloc);

                auto desired_iter = conn.c_desired_paths.find(pe.pe_path);
                if (desired_iter != conn.c_desired_paths.end()) {
                    report_error(this->get_display_path(pe.pe_path), pe.pe_msg);
                    if (!desired_iter->second.loo_follow) {
                        conn.c_desired_paths.erase(desired_iter);
                    }
                } else {
                    auto child_iter = conn.c_child_paths.find(pe.pe_path);

                    if (child_iter != conn.c_child_paths.end()
                        && !child_iter->second.loo_follow)
                    {
                        conn.c_child_paths.erase(child_iter);
                    }
                }

                auto remote_path = std::filesystem::absolute(
                                       std::filesystem::path(pe.pe_path))
                                       .relative_path();
                auto local_path = this->ht_local_path / remote_path;

                log_debug("removing %s", local_path.c_str());
                this->ht_active_files.erase(local_path);
                std::filesystem::remove_all(local_path);

                if (conn.c_desired_paths.empty()
                    && conn.c_child_paths.empty()) {
                    log_info("tailer(%s): all desired paths synced",
                             this->ht_netloc.c_str());
                    set_remote_state(this->ht_netloc,
                                     msg_level::ok,
                                     "Synced",
                                     lnav::console::user_message::ok(
                                         "all paths synced, disconnecting"));
                    return state_v{synced{}};
                }

                return std::move(this->ht_state);
            },
            [&](const tailer::packet_offer_block& pob) {
                log_debug("Got an offer: %s  %lld - %lld",
                          pob.pob_path.c_str(),
                          pob.pob_offset,
                          pob.pob_length);

                logfile_open_options_base loo;
                if (pob.pob_path == pob.pob_root_path) {
                    auto root_iter = conn.c_desired_paths.find(pob.pob_path);

                    if (root_iter == conn.c_desired_paths.end()) {
                        log_warning("ignoring unknown root: %s",
                                    pob.pob_root_path.c_str());
                        return std::move(this->ht_state);
                    }

                    loo = root_iter->second;
                } else {
                    auto child_iter = conn.c_child_paths.find(pob.pob_path);
                    if (child_iter == conn.c_child_paths.end()) {
                        auto root_iter
                            = conn.c_desired_paths.find(pob.pob_root_path);

                        if (root_iter == conn.c_desired_paths.end()) {
                            log_warning("ignoring child of unknown root: %s",
                                        pob.pob_root_path.c_str());
                            return std::move(this->ht_state);
                        }

                        // Copy, the root still owns its options and other
                        // children will need them as well.
                        conn.c_child_paths[pob.pob_path] = root_iter->second;
                        child_iter = conn.c_child_paths.find(pob.pob_path);
                    }

                    loo = child_iter->second;
                }

                update_tailer_description(
                    this->ht_netloc, conn.c_desired_paths, this->ht_uname);

                auto remote_path = std::filesystem::absolute(
                                       std::filesystem::path(pob.pob_path))
                                       .relative_path();
                auto local_path = this->ht_local_path / remote_path;
                auto open_res
                    = lnav::filesystem::open_file(local_path, O_RDONLY);

                if (this->ht_active_files.count(local_path) == 0) {
                    this->ht_active_files.insert(local_path);

                    auto custom_name = this->get_display_path(pob.pob_path);
                    isc::to<main_looper&, services::main_t>().send(
                        [local_path,
                         custom_name,
                         loo,
                         netloc = this->ht_netloc](auto& mlooper) {
                            auto& active_fc = lnav_data.ld_active_files;
                            auto lpath_str = local_path.string();

                            {
                                safe::WriteAccess<safe_scan_progress> sp(
                                    *active_fc.fc_progress);

                                sp->sp_tailers.erase(netloc);
                            }
                            if (active_fc.fc_file_names.count(lpath_str) > 0) {
                                log_debug("already in fc_file_names");
                                return;
                            }
                            if (active_fc.fc_closed_files.count(custom_name)
                                > 0) {
                                log_debug("in closed");
                                return;
                            }

                            file_collection fc;

                            fc.fc_file_names[lpath_str]
                                .with_filename(custom_name)
                                .with_source(logfile_name_source::REMOTE)
                                .with_follow(loo.loo_follow)
                                .with_visible_size_limit(256 * 1024);
                            update_active_files(fc);
                        });
                }

                if (open_res.isErr()) {
                    log_debug("file not found (%s), sending need block",
                              open_res.unwrapErr().c_str());
                    send_packet(conn.ht_to_child.get(),
                                TPT_NEED_BLOCK,
                                TPPT_STRING,
                                pob.pob_path.c_str(),
                                TPPT_DONE);
                    return std::move(this->ht_state);
                }

                auto fd = open_res.unwrap();
                struct stat st;

                if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode)) {
                    log_debug("path changed, sending need block");
                    std::filesystem::remove_all(local_path);
                    send_packet(conn.ht_to_child.get(),
                                TPT_NEED_BLOCK,
                                TPPT_STRING,
                                pob.pob_path.c_str(),
                                TPPT_DONE);
                    return std::move(this->ht_state);
                }

                if (st.st_size == pob.pob_offset) {
                    log_debug("local file is synced, sending need block");
                    send_packet(conn.ht_to_child.get(),
                                TPT_NEED_BLOCK,
                                TPPT_STRING,
                                pob.pob_path.c_str(),
                                TPPT_DONE);
                    return std::move(this->ht_state);
                }

                constexpr int64_t BUFFER_SIZE = 4 * 1024 * 1024;
                auto buffer = auto_mem<unsigned char>::malloc(BUFFER_SIZE);
                auto remaining = pob.pob_length;
                auto remaining_offset = pob.pob_offset;
                hash_frag thf;
                SHA256_CTX shactx;
                sha256_init(&shactx);

                log_debug("checking offer %s[%lld..+%lld]",
                          local_path.c_str(),
                          remaining_offset,
                          remaining);
                while (remaining > 0) {
                    auto nbytes = std::min(remaining, BUFFER_SIZE);
                    auto bytes_read
                        = pread(fd, buffer, nbytes, remaining_offset);
                    if (bytes_read == -1) {
                        log_debug(
                            "unable to read file, sending need block -- %s",
                            strerror(errno));
                        std::filesystem::remove_all(local_path);
                        break;
                    }
                    if (bytes_read == 0) {
                        break;
                    }
                    sha256_update(&shactx, buffer.in(), bytes_read);
                    remaining -= bytes_read;
                    remaining_offset += bytes_read;
                }

                if (remaining == 0) {
                    sha256_final(&shactx, thf.thf_hash);

                    if (thf == pob.pob_hash) {
                        log_debug("local file block is same, sending ack");
                        send_packet(conn.ht_to_child.get(),
                                    TPT_ACK_BLOCK,
                                    TPPT_STRING,
                                    pob.pob_path.c_str(),
                                    TPPT_INT64,
                                    pob.pob_offset,
                                    TPPT_INT64,
                                    pob.pob_length,
                                    TPPT_INT64,
                                    (int64_t) st.st_size,
                                    TPPT_DONE);
                        return std::move(this->ht_state);
                    }
                    log_debug("local file is different, sending need block");
                }
                send_packet(conn.ht_to_child.get(),
                            TPT_NEED_BLOCK,
                            TPPT_STRING,
                            pob.pob_path.c_str(),
                            TPPT_DONE);
                return std::move(this->ht_state);
            },
            [&](const tailer::packet_tail_block& ptb) {
                auto remote_path = std::filesystem::absolute(
                                       std::filesystem::path(ptb.ptb_path))
                                       .relative_path();
                auto local_path = this->ht_local_path / remote_path;

                log_debug("writing tail to: %lld/%ld %s",
                          ptb.ptb_offset,
                          ptb.ptb_bits.size(),
                          local_path.c_str());
                std::filesystem::create_directories(local_path.parent_path());
                auto create_res = lnav::filesystem::create_file(
                    local_path, O_WRONLY | O_APPEND | O_CREAT, 0600);

                if (create_res.isErr()) {
                    log_error("open: %s", create_res.unwrapErr().c_str());
                } else {
                    auto fd = create_res.unwrap();
                    ftruncate(fd, ptb.ptb_offset);
                    pwrite(fd,
                           ptb.ptb_bits.data(),
                           ptb.ptb_bits.size(),
                           ptb.ptb_offset);
                    auto mtime = std::filesystem::file_time_type{
                        std::chrono::seconds{ptb.ptb_mtime}};
                    // XXX This isn't atomic with the write...
                    std::filesystem::last_write_time(local_path, mtime);
                }
                return std::move(this->ht_state);
            },
            [&](const tailer::packet_synced& ps) {
                if (ps.ps_root_path == ps.ps_path) {
                    auto iter = conn.c_desired_paths.find(ps.ps_path);

                    if (iter != conn.c_desired_paths.end()) {
                        if (iter->second.loo_follow) {
                            conn.c_synced_desired_paths.insert(ps.ps_path);
                        } else {
                            log_info("synced desired path: %s",
                                     iter->first.c_str());
                            conn.c_desired_paths.erase(iter);
                        }
                    }
                } else {
                    auto iter = conn.c_child_paths.find(ps.ps_path);

                    if (iter != conn.c_child_paths.end()) {
                        if (iter->second.loo_follow) {
                            conn.c_synced_child_paths.insert(ps.ps_path);
                        } else {
                            log_info("synced child path: %s",
                                     iter->first.c_str());
                            conn.c_child_paths.erase(iter);
                        }
                    }
                }

                if (conn.c_desired_paths.empty()
                    && conn.c_child_paths.empty()) {
                    log_info("tailer(%s): all desired paths synced",
                             this->ht_netloc.c_str());
                    set_remote_state(this->ht_netloc,
                                     msg_level::ok,
                                     "Synced",
                                     lnav::console::user_message::ok(
                                         "all paths synced, disconnecting"));
                    return state_v{synced{}};
                } else if (!conn.c_initial_sync_done
                           && conn.c_desired_paths.size()
                               == conn.c_synced_desired_paths.size()
                           && conn.c_child_paths.size()
                               == conn.c_synced_child_paths.size())
                {
                    log_info("tailer(%s): all desired paths synced",
                             this->ht_netloc.c_str());
                    set_remote_state(this->ht_netloc,
                                     msg_level::ok,
                                     "Following",
                                     lnav::console::user_message::ok(
                                         "initial sync done, following "
                                         "changes"));
                    conn.c_initial_sync_done = true;

                    std::set<std::string> synced_files;
                    for (const auto& desired_pair : conn.c_desired_paths) {
                        synced_files.emplace(fmt::format(
                            FMT_STRING("{}{}"), ht_netloc, desired_pair.first));
                    }
                    isc::to<main_looper&, services::main_t>().send(
                        [file_set = std::move(synced_files)](auto& mlooper) {
                            file_collection fc;

                            fc.fc_synced_files = file_set;
                            update_active_files(fc);
                        });
                }

                return std::move(this->ht_state);
            },
            [&](const tailer::packet_link& pl) {
                auto remote_path = std::filesystem::absolute(
                                       std::filesystem::path(pl.pl_path))
                                       .relative_path();
                auto local_path = this->ht_local_path / remote_path;
                auto remote_link_path = std::filesystem::path(pl.pl_link_value);
                std::string link_path;

                if (remote_link_path.is_absolute()) {
                    auto local_link_path = this->ht_local_path
                        / remote_link_path.relative_path();

                    link_path = local_link_path.string();
                } else {
                    link_path = remote_link_path.string();
                }

                log_debug("symlinking %s -> %s",
                          local_path.c_str(),
                          link_path.c_str());
                std::filesystem::create_directories(local_path.parent_path());
                std::filesystem::remove_all(local_path);
                if (symlink(link_path.c_str(), local_path.c_str()) < 0) {
                    log_error("symlink failed: %s", strerror(errno));
                }

                if (pl.pl_root_path == pl.pl_path) {
                    auto iter = conn.c_desired_paths.find(pl.pl_path);

                    if (iter != conn.c_desired_paths.end()) {
                        if (iter->second.loo_follow) {
                            conn.c_synced_desired_paths.insert(pl.pl_path);
                        } else {
                            log_info("synced desired path: %s",
                                     iter->first.c_str());
                            conn.c_desired_paths.erase(iter);
                        }
                    }
                } else {
                    auto iter = conn.c_child_paths.find(pl.pl_path);

                    if (iter != conn.c_child_paths.end()) {
                        if (iter->second.loo_follow) {
                            conn.c_synced_child_paths.insert(pl.pl_path);
                        } else {
                            log_info("synced child path: %s",
                                     iter->first.c_str());
                            conn.c_child_paths.erase(iter);
                        }
                    }
                }

                return std::move(this->ht_state);
            },
            [&](const tailer::packet_preview_error& ppe) {
                isc::to<main_looper&, services::main_t>().send(
                    [ppe](auto& mlooper) {
                        if (lnav_data.ld_preview_generation != ppe.ppe_id) {
                            log_debug("preview ID mismatch: %lld != %lld",
                                      lnav_data.ld_preview_generation,
                                      ppe.ppe_id);
                            return;
                        }
                        lnav_data.ld_preview_status_source[0]
                            .get_description()
                            .set_cylon(false)
                            .clear();
                        lnav_data.ld_preview_source[0].clear();
                        lnav_data.ld_bottom_source.grep_error(ppe.ppe_msg);
                        lnav_data.ld_status[LNS_BOTTOM].set_needs_update();
                    });

                return std::move(this->ht_state);
            },
            [&](const tailer::packet_preview_data& ppd) {
                isc::to<main_looper&, services::main_t>().send(
                    [netloc = this->ht_netloc, ppd](auto& mlooper) {
                        if (lnav_data.ld_preview_generation != ppd.ppd_id) {
                            log_debug("preview ID mismatch: %lld != %lld",
                                      lnav_data.ld_preview_generation,
                                      ppd.ppd_id);
                            return;
                        }
                        std::string str(ppd.ppd_bits.begin(),
                                        ppd.ppd_bits.end());
                        lnav_data.ld_preview_status_source[0]
                            .get_description()
                            .set_cylon(false)
                            .set_value("For file: %s:%s",
                                       netloc.c_str(),
                                       ppd.ppd_path.c_str());
                        lnav_data.ld_status[LNS_PREVIEW0].set_needs_update();
                        lnav_data.ld_preview_source[0]
                            .replace_with(str)
                            .set_text_format(detect_text_format(str));
                    });
                return std::move(this->ht_state);
            },
            [&](const tailer::packet_possible_path& ppp) {
                log_debug("possible path: %s", ppp.ppp_path.c_str());
                auto full_path = fmt::format(
                    FMT_STRING("{}{}"), this->ht_netloc, ppp.ppp_path);

                isc::to<main_looper&, services::main_t>().send(
                    [full_path](auto& mlooper) {
                        static auto& prompt = lnav::prompt::get();

                        prompt.p_remote_paths.insert(full_path);
                    });
                return std::move(this->ht_state);
            });

        if (!this->ht_state.is<connected>()) {
            this->s_looping = false;
        }
    }
}

std::optional<std::chrono::milliseconds>
tailer::looper::host_tailer::compute_timeout(mstime_t current_time) const
{
    return 0s;
}

void
tailer::looper::host_tailer::stopped()
{
    if (this->ht_state.is<connected>()) {
        this->ht_state = disconnected();
    }
    if (this->ht_error_reader.joinable()) {
        this->ht_error_reader.join();
    }
}

std::string
tailer::looper::host_tailer::get_display_path(
    const std::string& remote_path) const
{
    return fmt::format(FMT_STRING("{}{}"), this->ht_netloc, remote_path);
}

void*
tailer::looper::host_tailer::run(worker* w)
{
    log_set_thread_prefix(
        fmt::format(FMT_STRING("tailer({})"), this->ht_netloc));

    return service_base::run(w);
}

auto_pid<process_state::finished>
tailer::looper::host_tailer::connected::close() &&
{
    this->ht_to_child.reset();
    this->ht_from_child.reset();

    return std::move(this->ht_child).wait_for_child();
}

void
tailer::looper::child_finished(std::shared_ptr<service_base> child)
{
    auto child_tailer = std::static_pointer_cast<host_tailer>(child);

    for (auto iter = this->l_remotes.begin(); iter != this->l_remotes.end();
         ++iter)
    {
        if (iter->second != child_tailer) {
            continue;
        }

        auto netloc_iter = this->l_netlocs_to_paths.find(iter->first);
        if (child_tailer->is_synced()) {
            log_info("synced with netloc '%s', removing", iter->first.c_str());

            if (netloc_iter != this->l_netlocs_to_paths.end()) {
                netloc_iter->second.send_synced_to_main(netloc_iter->first);
                this->l_netlocs_to_paths.erase(netloc_iter);
            }
        } else if (netloc_iter != this->l_netlocs_to_paths.end()
                   && netloc_iter->second.has_unfollowed_paths())
        {
            log_info("disconnected from netloc '%s', not retrying",
                     iter->first.c_str());
            netloc_iter->second.send_synced_to_main(netloc_iter->first);
            this->l_netlocs_to_paths.erase(netloc_iter);
        } else if (netloc_iter != this->l_netlocs_to_paths.end()) {
            log_info("disconnected from netloc '%s', retrying in %lld seconds",
                     iter->first.c_str(),
                     (long long) std::chrono::duration_cast<std::chrono::seconds>(
                         HOST_RETRY_DELAY)
                         .count());
            netloc_iter->second.rpq_next_attempt_time
                = std::chrono::steady_clock::now() + HOST_RETRY_DELAY;
            remote_log(iter->first,
                       lnav::console::user_message::raw(fmt::format(
                           FMT_STRING("retrying in {}"), HOST_RETRY_DELAY)));
        }
        lnav_data.ld_active_files.fc_progress->writeAccess()->sp_tailers.erase(
            iter->first);
        this->l_remotes.erase(iter);
        return;
    }
}

bool
tailer::looper::remote_path_queue::has_unfollowed_paths() const
{
    auto unfollowed
        = [](const auto& pair) { return !pair.second.loo_follow; };

    return std::any_of(
               this->rpq_new_paths.begin(), this->rpq_new_paths.end(), unfollowed)
        || std::any_of(this->rpq_existing_paths.begin(),
                       this->rpq_existing_paths.end(),
                       unfollowed);
}

void
tailer::looper::remote_path_queue::send_synced_to_main(
    const std::string& netloc)
{
    std::set<std::string> synced_files;

    for (const auto& pair : this->rpq_new_paths) {
        if (!pair.second.loo_follow) {
            synced_files.emplace(
                fmt::format(FMT_STRING("{}{}"), netloc, pair.first));
        }
    }
    for (const auto& pair : this->rpq_existing_paths) {
        if (!pair.second.loo_follow) {
            synced_files.emplace(
                fmt::format(FMT_STRING("{}{}"), netloc, pair.first));
        }
    }

    isc::to<main_looper&, services::main_t>().send(
        [file_set = std::move(synced_files)](auto& mlooper) {
            file_collection fc;

            fc.fc_synced_files = file_set;
            update_active_files(fc);
        });
}

void
tailer::looper::report_error(std::string path, std::string msg)
{
    log_error("reporting error: %s -- %s", path.c_str(), msg.c_str());
    isc::to<main_looper&, services::main_t>().send([=](auto& mlooper) {
        file_collection fc;
        auto um = lnav::console::user_message::error(
                      attr_line_t("unable to open remote path ")
                          .append_quoted(lnav::roles::file(path)))
                      .with_reason(msg);

        fc.fc_name_to_stubs->writeAccess()->emplace(path,
                                                    file_stub_info{
                                                        path,
                                                        std::nullopt,
                                                        std::nullopt,
                                                        um.move(),
                                                    });
        update_active_files(fc);
        lnav_data.ld_active_files.fc_progress->writeAccess()->sp_tailers.erase(
            path);
    });
}

std::future<void>
tailer::cleanup_cache()
{
    return std::async(
        std::launch::async, +[]() {
            auto now = std::filesystem::file_time_type::clock::now();
            auto cache_path = remote_cache_path();
            const auto& cfg = injector::get<const config&>();
            std::vector<std::filesystem::path> to_remove;
            std::error_code ec;

            log_debug("cache-ttl %lld", cfg.c_cache_ttl.count());
            for (const auto& entry :
                 std::filesystem::directory_iterator(cache_path, ec))
            {
                auto mtime = std::filesystem::last_write_time(entry.path());
                auto exp_time = mtime + cfg.c_cache_ttl;
                if (now < exp_time) {
                    continue;
                }

                to_remove.emplace_back(entry.path());
            }

            for (auto& entry : to_remove) {
                log_debug("removing cached remote: %s", entry.c_str());
                std::filesystem::remove_all(entry);
            }
        });
}
