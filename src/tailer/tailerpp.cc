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

#include "tailerpp.hh"

#include <unistd.h>
#include <zlib.h>

namespace tailer {

int
readall(int sock, void* buf, size_t len)
{
    char* cbuf = (char*) buf;
    off_t offset = 0;

    while (len > 0) {
        ssize_t rc = read(sock, &cbuf[offset], len);

        if (rc == -1) {
            switch (errno) {
                case EAGAIN:
                case EINTR:
                    break;
                default:
                    return -1;
            }
        } else if (rc == 0) {
            errno = EIO;
            return -1;
        } else {
            len -= rc;
            offset += rc;
        }
    }

    return 0;
}

Result<void, std::string>
read_bits(int fd, std::vector<uint8_t>& bits)
{
    tailer_packet_payload_type_t payload_type;

    if (readall(fd, &payload_type, sizeof(payload_type)) == -1) {
        return Err(fmt::format(FMT_STRING("unable to read payload type: {}"),
                               strerror(errno)));
    }

    switch (payload_type) {
        case TPPT_BITS: {
            int32_t length;

            if (readall(fd, &length, sizeof(length)) == -1) {
                return Err(fmt::format(
                    FMT_STRING("unable to read content length: {}"),
                    strerror(errno)));
            }
            if (length < 0 || length > MAX_PAYLOAD_LENGTH) {
                return Err(fmt::format(FMT_STRING("invalid content length: {}"),
                                       length));
            }
            bits.resize(length);
            if (readall(fd, bits.data(), length) == -1) {
                return Err(
                    fmt::format(FMT_STRING("unable to read content -- {}"),
                                strerror(errno)));
            }
            return Ok();
        }
        case TPPT_DEFLATED_BITS: {
            int32_t lengths[2];

            if (readall(fd, lengths, sizeof(lengths)) == -1) {
                return Err(fmt::format(
                    FMT_STRING("unable to read content lengths: {}"),
                    strerror(errno)));
            }

            const auto length = lengths[0];
            const auto deflated_length = lengths[1];
            if (length < 0 || length > MAX_PAYLOAD_LENGTH
                || deflated_length < 0 || deflated_length > MAX_PAYLOAD_LENGTH)
            {
                return Err(
                    fmt::format(FMT_STRING("invalid content lengths: {}/{}"),
                                length,
                                deflated_length));
            }

            std::vector<uint8_t> deflated(deflated_length);
            if (readall(fd, deflated.data(), deflated_length) == -1) {
                return Err(
                    fmt::format(FMT_STRING("unable to read content -- {}"),
                                strerror(errno)));
            }

            bits.resize(length);
            uLongf dest_len = length;
            auto rc = uncompress(
                bits.data(), &dest_len, deflated.data(), deflated_length);
            if (rc != Z_OK || dest_len != (uLongf) length) {
                return Err(fmt::format(
                    FMT_STRING("unable to decompress content: rc={}; "
                               "length={}; expected={}"),
                    rc,
                    dest_len,
                    length));
            }
            return Ok();
        }
        default:
            return Err(fmt::format(
                FMT_STRING("payload-type mismatch, got: {}; expected: {}"),
                (int) payload_type,
                (int) TPPT_BITS));
    }
}

Result<packet, std::string>
read_packet(int fd)
{
    tailer_packet_type_t type;

    if (readall(fd, &type, sizeof(type)) == -1) {
        return Ok(packet{packet_eof{}});
    }
    switch (type) {
        case TPT_ERROR: {
            packet_error pe;

            TRY(read_payloads_into(fd, pe.pe_path, pe.pe_msg));
            return Ok(packet{pe});
        }
        case TPT_ANNOUNCE: {
            packet_announce pa;

            TRY(read_payloads_into(fd, pa.pa_uname));
            return Ok(packet{pa});
        }
        case TPT_OFFER_BLOCK: {
            packet_offer_block pob;

            TRY(read_payloads_into(fd,
                                   pob.pob_root_path,
                                   pob.pob_path,
                                   pob.pob_mtime,
                                   pob.pob_offset,
                                   pob.pob_length,
                                   pob.pob_hash));
            return Ok(packet{pob});
        }
        case TPT_TAIL_BLOCK: {
            packet_tail_block ptb;

            TRY(read_payloads_into(fd,
                                   ptb.ptb_root_path,
                                   ptb.ptb_path,
                                   ptb.ptb_mtime,
                                   ptb.ptb_offset,
                                   ptb.ptb_bits));
            return Ok(packet{ptb});
        }
        case TPT_SYNCED: {
            packet_synced ps;

            TRY(read_payloads_into(fd, ps.ps_root_path, ps.ps_path));
            return Ok(packet{ps});
        }
        case TPT_LINK_BLOCK: {
            packet_link pl;

            TRY(read_payloads_into(
                fd, pl.pl_root_path, pl.pl_path, pl.pl_link_value));
            return Ok(packet{pl});
        }
        case TPT_PREVIEW_ERROR: {
            packet_preview_error ppe;

            TRY(read_payloads_into(fd, ppe.ppe_id, ppe.ppe_path, ppe.ppe_msg));
            return Ok(packet{ppe});
        }
        case TPT_PREVIEW_DATA: {
            packet_preview_data ppd;

            TRY(read_payloads_into(fd, ppd.ppd_id, ppd.ppd_path, ppd.ppd_bits));
            return Ok(packet{ppd});
        }
        case TPT_POSSIBLE_PATH: {
            packet_possible_path ppp;

            TRY(read_payloads_into(fd, ppp.ppp_path));
            return Ok(packet{ppp});
        }
        default:
            return Err(
                fmt::format(FMT_STRING("unexpected packet type on the wire: {}"),
                            (int) type));
    }
}

}  // namespace tailer
