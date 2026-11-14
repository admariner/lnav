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

#include <iostream>

#include "base/humanize.network.hh"
#include "config.h"
#include "doctest/doctest.h"

TEST_CASE("humanize::network::path")
{
    {
        auto rp_opt = humanize::network::path::from_str(
            string_fragment::from_const("foobar"));
        CHECK(!rp_opt);
    }
    {
        auto rp_opt = humanize::network::path::from_str(
            string_fragment::from_const("dean@foobar/bar"));
        CHECK(!rp_opt);
    }

    {
        auto rp_opt = humanize::network::path::from_str(
            string_fragment::from_const("dean@host1.example.com:/var/log"));
        CHECK(rp_opt.has_value());

        auto rp = *rp_opt;
        CHECK(rp.p_locality.l_username.has_value());
        CHECK(rp.p_locality.l_username.value() == "dean");
        CHECK(rp.p_locality.l_hostname == "host1.example.com");
        CHECK(!rp.p_locality.l_service.has_value());
        CHECK(rp.p_path == "/var/log");
    }

    {
        auto rp_opt
            = humanize::network::path::from_str(string_fragment::from_const(
                "dean@[fe80::184f:c67:baf1:fe02%en0]:/var/log"));
        CHECK(rp_opt.has_value());

        auto rp = *rp_opt;
        CHECK(rp.p_locality.l_username.has_value());
        CHECK(rp.p_locality.l_username.value() == "dean");
        CHECK(rp.p_locality.l_hostname == "fe80::184f:c67:baf1:fe02%en0");
        CHECK(!rp.p_locality.l_service.has_value());
        CHECK(rp.p_path == "/var/log");

        CHECK(fmt::to_string(rp.p_locality)
              == "dean@[fe80::184f:c67:baf1:fe02%en0]");
    }

    {
        auto rp_opt
            = humanize::network::path::from_str(string_fragment::from_const(
                "[fe80::184f:c67:baf1:fe02%en0]:/var/log"));
        CHECK(rp_opt.has_value());

        auto rp = *rp_opt;
        CHECK(!rp.p_locality.l_username.has_value());
        CHECK(rp.p_locality.l_hostname == "fe80::184f:c67:baf1:fe02%en0");
        CHECK(!rp.p_locality.l_service.has_value());
        CHECK(rp.p_path == "/var/log");

        CHECK(fmt::to_string(rp.p_locality)
              == "[fe80::184f:c67:baf1:fe02%en0]");
    }

    {
        auto rp_opt = humanize::network::path::from_str(
            string_fragment::from_const("host1.example.com:/var/log"));
        CHECK(rp_opt.has_value());

        auto rp = *rp_opt;
        CHECK(!rp.p_locality.l_username.has_value());
        CHECK(rp.p_locality.l_hostname == "host1.example.com");
        CHECK(!rp.p_locality.l_service.has_value());
        CHECK(rp.p_path == "/var/log");
    }

    {
        auto rp_opt = humanize::network::path::from_str(
            string_fragment::from_const("host1.example.com:"));
        CHECK(rp_opt.has_value());

        auto rp = *rp_opt;
        CHECK(!rp.p_locality.l_username.has_value());
        CHECK(rp.p_locality.l_hostname == "host1.example.com");
        CHECK(!rp.p_locality.l_service.has_value());
        CHECK(rp.p_path == ".");
    }
}

TEST_CASE("humanize::network::path container")
{
    {
        auto rp_opt = humanize::network::path::from_str(
            string_fragment::from_const(
                "tstack-jetty11-2/snaplogic-groundplex-jcc-7d6495d5d5-q8ktf:"
                "/opt/snaplogic/run/log/jcc.json"));
        REQUIRE(rp_opt.has_value());

        auto rp = *rp_opt;
        CHECK(rp.p_locality.l_transport == network::transport_t::kubectl);
        CHECK(rp.p_locality.l_namespace.value() == "tstack-jetty11-2");
        CHECK(rp.p_locality.l_hostname
              == "snaplogic-groundplex-jcc-7d6495d5d5-q8ktf");
        CHECK(!rp.p_locality.l_container.has_value());
        CHECK(!rp.p_locality.l_username.has_value());
        CHECK(rp.p_path == "/opt/snaplogic/run/log/jcc.json");
        CHECK(fmt::to_string(rp.p_locality)
              == "tstack-jetty11-2/snaplogic-groundplex-jcc-7d6495d5d5-q8ktf");
    }

    {
        auto rp_opt = humanize::network::path::from_str(
            string_fragment::from_const("default/web.v1/nginx:/var/log"));
        REQUIRE(rp_opt.has_value());

        auto rp = *rp_opt;
        CHECK(rp.p_locality.l_transport == network::transport_t::kubectl);
        CHECK(rp.p_locality.l_namespace.value() == "default");
        CHECK(rp.p_locality.l_hostname == "web.v1");
        CHECK(rp.p_locality.l_container.value() == "nginx");
        CHECK(rp.p_path == "/var/log");
        CHECK(fmt::to_string(rp) == "default/web.v1/nginx:/var/log");

        auto home_str = fmt::to_string(rp.home());
        CHECK(home_str == "default/web.v1/nginx:");
        auto home_opt
            = humanize::network::path::from_str(string_fragment::from_str(home_str));
        REQUIRE(home_opt.has_value());
        CHECK(fmt::to_string(home_opt->p_locality) == "default/web.v1/nginx");
        CHECK(home_opt->p_path == ".");
    }

    {
        auto rp_opt = humanize::network::path::from_str(
            string_fragment::from_const("default/web:"));
        REQUIRE(rp_opt.has_value());
        CHECK(rp_opt->p_path == ".");
    }

    {
        auto rp_opt = humanize::network::path::from_str(
            string_fragment::from_const("host1.example.com:/var/log"));
        REQUIRE(rp_opt.has_value());
        CHECK(rp_opt->p_locality.l_transport == network::transport_t::ssh);
    }

    for (const auto* str : {
             "Logs/foo:bar",
             "./a/b:c",
             "a/b/c/d:x",
             "/abs/pod:x",
             "ns/-pod:x",
             "ns/pod-:x",
             "ns/pod/c.d:x",
             "dean@ns/pod:x",
         })
    {
        INFO(str);
        CHECK(!humanize::network::path::from_str(
                   string_fragment::from_c_str(str))
                   .has_value());
    }

    {
        auto long_ns = std::string(64, 'a') + "/pod:/x";
        CHECK(!humanize::network::path::from_str(string_fragment::from_str(long_ns))
                   .has_value());
        auto ok_ns = std::string(63, 'a') + "/pod:/x";
        CHECK(humanize::network::path::from_str(string_fragment::from_str(ok_ns))
                  .has_value());
    }
}
