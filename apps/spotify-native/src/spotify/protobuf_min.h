/*
 * protobuf_min.h — minimal protobuf wire-format encode/decode.
 *
 * Not a general protobuf library — just enough to build ClientHello /
 * ClientResponsePlaintext and walk APResponseMessage's fields, per
 * the schema in research/auth/ (extracted from librespot's
 * protocol/proto/keyexchange.proto). Hand-rolled deliberately to
 * avoid a protobuf-c dependency for a handful of small, fixed
 * messages — consistent with the project's "every dependency must
 * earn its place" principle.
 *
 * Wire format reference (proto2, only what we need):
 *   tag      = varint((field_number << 3) | wire_type)
 *   varint   = 7 bits per byte, LSB-first groups, MSB=1 means "more bytes follow"
 *   wire_type 0 = varint   (int32/uint32/uint64/bool/enum)
 *   wire_type 2 = length-delimited (bytes/string/embedded message)
 *
 * Note: Spotify's internal .proto schemas number fields in multiples
 * of 10 (first field = 10, not 1) -- that's a real quirk of their
 * schema, not a mistake in ours; field numbers below match
 * research/auth/ and the upstream .proto exactly.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* ── Encoding ─────────────────────────────────────────────────────────────── */

void pb_write_varint        (GByteArray *buf, guint64 value);
void pb_write_tag           (GByteArray *buf, guint32 field_num, guint32 wire_type);
void pb_write_varint_field  (GByteArray *buf, guint32 field_num, guint64 value);
void pb_write_bytes_field   (GByteArray *buf, guint32 field_num, const guint8 *data, gsize len);

/* Wraps an already-built submessage (its raw encoded bytes) as a
 * length-delimited field in the parent buffer. */
void pb_write_message_field (GByteArray *buf, guint32 field_num,
                             const guint8 *submsg_data, gsize submsg_len);

/* ── Decoding ─────────────────────────────────────────────────────────────── */

typedef enum {
  PB_WIRE_VARINT = 0,
  PB_WIRE_FIXED64 = 1,
  PB_WIRE_LENGTH_DELIMITED = 2,
  PB_WIRE_FIXED32 = 5,
} PbWireType;

/* Reads one field's tag + value starting at *pos, advancing *pos past
 * it. For length-delimited fields, *out_data/*out_len point directly
 * into buf (no copy). For varints, the value lands in *out_varint.
 * Returns FALSE if there's no more data or the buffer is malformed. */
gboolean pb_read_field (const guint8 *buf, gsize len, gsize *pos,
                        guint32 *field_num, PbWireType *wire_type,
                        const guint8 **out_data, gsize *out_len,
                        guint64 *out_varint);

/* Convenience: scan all top-level fields of a message looking for the
 * first occurrence of want_field_num with a length-delimited value.
 * Returns FALSE if not found. This is enough for walking the known,
 * fixed paths we need in APResponseMessage -- not a general accessor. */
gboolean pb_find_bytes_field (const guint8 *buf, gsize len, guint32 want_field_num,
                              const guint8 **out_data, gsize *out_len);

gboolean pb_find_varint_field (const guint8 *buf, gsize len, guint32 want_field_num,
                               guint64 *out_value);

G_END_DECLS
