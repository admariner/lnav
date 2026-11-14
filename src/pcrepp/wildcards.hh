/**
 * Copyright (c) 2026, Timothy Stack
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

#ifndef lnav_wildcards_hh
#define lnav_wildcards_hh

#include <optional>
#include <string>

#include "base/intern_string.hh"

/**
 * SQLite's LIKE and GLOB patterns as PCRE patterns, for matching text with
 * them in place, as PCRE does a string_fragment, instead of copying it into
 * NUL-terminated strings for sqlite3_strlike() and sqlite3_strglob().
 */
namespace lnav::wildcards {

/**
 * A PCRE pattern (UTF mode) for SQLite's `LIKE` with [pattern]: `%` is any
 * run of characters, `_` any one, the rest literal, and case is ignored.  It
 * matches every string LIKE does, and some more (it folds case beyond ASCII
 * too, and ignores `PRAGMA case_sensitive_like`), so it's for ruling out
 * strings before SQLite applies LIKE itself.  nullopt when [pattern] isn't
 * valid UTF-8.
 */
std::optional<std::string> like_to_pcre(string_fragment pattern);

/**
 * A PCRE pattern (UTF mode) for SQLite's `GLOB` with [pattern], which it
 * matches exactly: `*` is any run of characters, `?` any one, `[...]` a set
 * (`^` first negates it, a `]` first is a member, `-` makes a range unless
 * it's first or last), case matters, and there are no escapes.  nullopt when
 * [pattern] isn't valid UTF-8 or has a set with no end, which GLOB matches
 * nothing with.
 */
std::optional<std::string> glob_to_pcre(string_fragment pattern);

}  // namespace lnav::wildcards

#endif
