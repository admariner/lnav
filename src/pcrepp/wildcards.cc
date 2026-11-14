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

#include <cctype>

#include "wildcards.hh"

#include "ww898/cp_utf8.hpp"

namespace lnav::wildcards {

namespace {

/**
 * Turns a LIKE or GLOB pattern into a PCRE one as it reads it: the pattern's
 * characters one at a time, as ww898's read() decodes them, and the PCRE
 * pattern written as it goes.  A run of `%` or `*` at the start or end
 * leaves that end unanchored instead of being `\A.*` or `.*\z`, which PCRE
 * would anchor and backtrack through rather than search for what's between,
 * and adjacent runs are one.
 */
class pattern_translator {
public:
    pattern_translator(string_fragment pattern, const char* flags)
        : pt_pattern(pattern), pt_re(flags)
    {
    }

    struct character {
        uint32_t c_codepoint;
        /** The character's bytes in the pattern. */
        string_fragment c_bytes;
    };

    bool done() const { return this->pt_index >= this->pt_pattern.length(); }

    /**
     * The next byte, unread, or 0 at the end.  Everything the syntax looks
     * ahead for is ASCII, which no byte of a multibyte character can be.
     */
    char peek() const
    {
        return this->done() ? '\0' : this->pt_pattern.data()[this->pt_index];
    }

    /** The next character, or nullopt when it isn't valid UTF-8. */
    std::optional<character> next()
    {
        const auto start = this->pt_index;
        const auto read_res = ww898::utf::utf8::read([this]() -> uint8_t {
            // Past the end reads as 0, which isn't a continuation byte, so a
            // sequence cut short is an error instead of a read past the end.
            const auto index = this->pt_index++;
            return index < (size_t) this->pt_pattern.length()
                ? (uint8_t) this->pt_pattern.data()[index]
                : 0;
        });
        // read() takes 5- and 6-byte forms, which aren't Unicode.
        if (read_res.isErr() || read_res.unwrap() > 0x10FFFF) {
            return std::nullopt;
        }
        return character{
            read_res.unwrap(),
            this->pt_pattern.sub_range(start, this->pt_index),
        };
    }

    /** A `%` or `*`: written as `.*` before what follows, if anything does. */
    void any_run() { this->pt_pending_run = true; }

    /**
     * The output, to append the next piece of the pattern to, after `\A` if
     * it's the first piece and no run came before it, or after `.*` if a run
     * came between it and the one before.
     */
    std::string& piece()
    {
        if (!this->pt_started) {
            if (!this->pt_pending_run) {
                this->pt_re.append("\\A");
            }
            this->pt_started = true;
        } else if (this->pt_pending_run) {
            this->pt_re.append(".*");
        }
        this->pt_pending_run = false;

        return this->pt_re;
    }

    /** Appends [ch] to [re], matched literally. */
    static void append_literal(std::string& re, const character& ch)
    {
        // PCRE takes a backslash before any ASCII character that isn't a
        // letter or digit as that character, inside a set too.
        if (ch.c_codepoint < 0x80 && !isalnum((int) ch.c_codepoint)) {
            re.push_back('\\');
        }
        re.append(ch.c_bytes.data(), ch.c_bytes.length());
    }

    /** The PCRE pattern, with `\z` unless it ended in a run. */
    std::string finish() &&
    {
        if (!this->pt_started) {
            // Only runs, which match anything, or nothing at all.
            if (!this->pt_pending_run) {
                this->pt_re.append("\\A\\z");
            }
        } else if (!this->pt_pending_run) {
            this->pt_re.append("\\z");
        }
        return std::move(this->pt_re);
    }

private:
    string_fragment pt_pattern;
    size_t pt_index{0};
    std::string pt_re;
    /** Whether a piece has been written (after `\A`, if any). */
    bool pt_started{false};
    /** Whether a `%` or `*` run came after the last piece. */
    bool pt_pending_run{false};
};

}  // namespace

std::optional<std::string>
like_to_pcre(string_fragment pattern)
{
    // (?s): % and _ match a newline too.  (?i): LIKE ignores ASCII case.
    pattern_translator pt(pattern, "(?si)");

    while (!pt.done()) {
        const auto ch = pt.next();
        if (!ch) {
            return std::nullopt;
        }
        switch (ch->c_codepoint) {
            case '%':
                pt.any_run();
                break;
            case '_':
                pt.piece().push_back('.');
                break;
            default:
                pattern_translator::append_literal(pt.piece(), *ch);
                break;
        }
    }

    return std::move(pt).finish();
}

std::optional<std::string>
glob_to_pcre(string_fragment pattern)
{
    pattern_translator pt(pattern, "(?s)");

    while (!pt.done()) {
        const auto ch = pt.next();
        if (!ch) {
            return std::nullopt;
        }
        switch (ch->c_codepoint) {
            case '*':
                pt.any_run();
                break;
            case '?':
                pt.piece().push_back('.');
                break;
            case '[': {
                // As sqlite3's patternCompare() reads a set.  A set that
                // doesn't end means no pattern, so what was written of it
                // doesn't matter.
                auto& re = pt.piece();
                re.push_back('[');
                if (pt.peek() == '^') {
                    pt.next();
                    re.push_back('^');
                }
                auto first = true;
                // Whether the member before was one character, which a '-'
                // makes the start of a range (sqlite's prior_c, which a
                // range resets).
                auto prior_char = false;
                auto closed = false;
                while (!pt.done()) {
                    const auto member = pt.next();
                    if (!member) {
                        return std::nullopt;
                    }
                    if (member->c_codepoint == ']' && !first) {
                        closed = true;
                        break;
                    }
                    if (member->c_codepoint == '-' && prior_char && !pt.done()
                        && pt.peek() != ']')
                    {
                        // A range from the member before.
                        const auto range_end = pt.next();
                        if (!range_end) {
                            return std::nullopt;
                        }
                        re.push_back('-');
                        pattern_translator::append_literal(re, *range_end);
                        prior_char = false;
                    } else {
                        pattern_translator::append_literal(re, *member);
                        prior_char = true;
                    }
                    first = false;
                }
                if (!closed) {
                    // GLOB matches nothing with a set that doesn't end.
                    return std::nullopt;
                }
                re.push_back(']');
                break;
            }
            default:
                pattern_translator::append_literal(pt.piece(), *ch);
                break;
        }
    }

    return std::move(pt).finish();
}

}  // namespace lnav::wildcards
