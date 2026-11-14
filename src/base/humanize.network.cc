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

#include <optional>

#include "humanize.network.hh"

#include "config.h"
#include "itertools.hh"
#include "pcrepp/pcre2pp.hh"

namespace humanize::network::path {

/**
 * Parse a kubectl-cp-style container path: namespace/pod[/container]:path
 */
static std::optional<::network::path>
container_from_str(string_fragment sf)
{
    static const auto CONTAINER_PATTERN = lnav::pcre2pp::code::from_const(
        "^(?<ns>[a-z0-9](?:[-a-z0-9]*[a-z0-9])?)"
        "/(?<pod>[a-z0-9](?:[-a-z0-9.]*[a-z0-9])?)"
        "(?:/(?<container>[a-z0-9](?:[-a-z0-9]*[a-z0-9])?))?:"
        "(?<path>.*)$");
    static constexpr size_t MAX_LABEL_LEN = 63;
    static constexpr size_t MAX_SUBDOMAIN_LEN = 253;
    thread_local auto CONTAINER_MATCH_DATA
        = CONTAINER_PATTERN.create_match_data();

    auto match_res = CONTAINER_PATTERN.capture_from(sf)
                         .into(CONTAINER_MATCH_DATA)
                         .matches()
                         .ignore_error();

    if (!match_res) {
        return std::nullopt;
    }

    const auto ns = *CONTAINER_MATCH_DATA["ns"];
    const auto pod = *CONTAINER_MATCH_DATA["pod"];
    const auto container = CONTAINER_MATCH_DATA["container"];
    if (ns.length() > (int) MAX_LABEL_LEN
        || pod.length() > (int) MAX_SUBDOMAIN_LEN
        || (container && container->length() > (int) MAX_LABEL_LEN))
    {
        return std::nullopt;
    }

    auto path = *CONTAINER_MATCH_DATA["path"];
    if (path.empty()) {
        path = string_fragment::from_const(".");
    }
    return ::network::path{
        ::network::locality::for_container(
            ns.to_string(),
            pod.to_string(),
            container | lnav::itertools::map([](auto sf) {
                return sf.to_string();
            })),
        path.to_string(),
    };
}

std::optional<::network::path>
from_str(string_fragment sf)
{
    static const auto REMOTE_PATTERN = lnav::pcre2pp::code::from_const(
        "^(?:(?<username>[\\w\\._\\-]+)@)?"
        "(?:\\[(?<ipv6>[^\\]]+)\\]|(?<hostname>[^\\[/:]{2,})):"
        "(?<path>.*)$");
    thread_local auto REMOTE_MATCH_DATA = REMOTE_PATTERN.create_match_data();

    auto match_res = REMOTE_PATTERN.capture_from(sf)
                         .into(REMOTE_MATCH_DATA)
                         .matches()
                         .ignore_error();

    if (!match_res) {
        return container_from_str(sf);
    }

    const auto username = REMOTE_MATCH_DATA["username"]
        | lnav::itertools::map([](auto sf) { return sf.to_string(); });
    const auto ipv6 = REMOTE_MATCH_DATA["ipv6"];
    const auto hostname = REMOTE_MATCH_DATA["hostname"];
    const auto locality_hostname = ipv6 ? ipv6.value() : hostname.value();
    auto path = *REMOTE_MATCH_DATA["path"];

    if (path.empty()) {
        path = string_fragment::from_const(".");
    }
    return ::network::path{
        {username, locality_hostname.to_string(), std::nullopt},
        path.to_string(),
    };
}

}  // namespace humanize::network::path