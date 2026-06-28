/*
 * protobuf_min.c — minimal protobuf wire-format implementation.
 * See protobuf_min.h for scope/rationale.
 */

#include "protobuf_min.h"

/* ── Encoding ─────────────────────────────────────────────────────────────── */

void
pb_write_varint (GByteArray *buf, guint64 value)
{
  while (value >= 0x80) {
    guint8 b = (guint8) (value & 0x7f) | 0x80;
    g_byte_array_append (buf, &b, 1);
    value >>= 7;
  }
  guint8 last = (guint8) (value & 0x7f);
  g_byte_array_append (buf, &last, 1);
}

void
pb_write_tag (GByteArray *buf, guint32 field_num, guint32 wire_type)
{
  pb_write_varint (buf, ((guint64) field_num << 3) | (guint64) wire_type);
}

void
pb_write_varint_field (GByteArray *buf, guint32 field_num, guint64 value)
{
  pb_write_tag (buf, field_num, PB_WIRE_VARINT);
  pb_write_varint (buf, value);
}

void
pb_write_bytes_field (GByteArray *buf, guint32 field_num, const guint8 *data, gsize len)
{
  pb_write_tag (buf, field_num, PB_WIRE_LENGTH_DELIMITED);
  pb_write_varint (buf, (guint64) len);
  if (len > 0)
    g_byte_array_append (buf, data, (guint) len);
}

void
pb_write_message_field (GByteArray *buf, guint32 field_num,
                        const guint8 *submsg_data, gsize submsg_len)
{
  /* Wire-format-wise, an embedded message is just a length-delimited
   * blob -- identical mechanics to a bytes field. */
  pb_write_bytes_field (buf, field_num, submsg_data, submsg_len);
}

/* ── Decoding ─────────────────────────────────────────────────────────────── */

static gboolean
pb_read_varint_raw (const guint8 *buf, gsize len, gsize *pos, guint64 *out)
{
  guint64 result = 0;
  int shift = 0;
  while (*pos < len) {
    guint8 b = buf[*pos];
    (*pos)++;
    result |= ((guint64) (b & 0x7f)) << shift;
    if (!(b & 0x80)) {
      *out = result;
      return TRUE;
    }
    shift += 7;
    if (shift >= 64)
      return FALSE;  /* malformed: varint too long */
  }
  return FALSE;  /* ran out of buffer mid-varint */
}

gboolean
pb_read_field (const guint8 *buf, gsize len, gsize *pos,
              guint32 *field_num, PbWireType *wire_type,
              const guint8 **out_data, gsize *out_len,
              guint64 *out_varint)
{
  if (*pos >= len) return FALSE;

  guint64 tag;
  if (!pb_read_varint_raw (buf, len, pos, &tag)) return FALSE;

  *field_num = (guint32) (tag >> 3);
  *wire_type = (PbWireType) (tag & 0x7);

  if (out_data)   *out_data   = NULL;
  if (out_len)    *out_len    = 0;
  if (out_varint) *out_varint = 0;

  switch (*wire_type) {
    case PB_WIRE_VARINT: {
      guint64 v;
      if (!pb_read_varint_raw (buf, len, pos, &v)) return FALSE;
      if (out_varint) *out_varint = v;
      return TRUE;
    }
    case PB_WIRE_LENGTH_DELIMITED: {
      guint64 flen;
      if (!pb_read_varint_raw (buf, len, pos, &flen)) return FALSE;
      if (*pos + (gsize) flen > len) return FALSE;  /* truncated */
      if (out_data) *out_data = buf + *pos;
      if (out_len)  *out_len  = (gsize) flen;
      *pos += (gsize) flen;
      return TRUE;
    }
    case PB_WIRE_FIXED64:
      if (*pos + 8 > len) return FALSE;
      *pos += 8;
      return TRUE;
    case PB_WIRE_FIXED32:
      if (*pos + 4 > len) return FALSE;
      *pos += 4;
      return TRUE;
    default:
      return FALSE;  /* unsupported/obsolete wire type (group start/end) */
  }
}

gboolean
pb_find_bytes_field (const guint8 *buf, gsize len, guint32 want_field_num,
                     const guint8 **out_data, gsize *out_len)
{
  gsize pos = 0;
  guint32 field_num;
  PbWireType wire_type;
  const guint8 *data;
  gsize dlen;
  guint64 vint;

  while (pb_read_field (buf, len, &pos, &field_num, &wire_type, &data, &dlen, &vint)) {
    if (field_num == want_field_num && wire_type == PB_WIRE_LENGTH_DELIMITED) {
      *out_data = data;
      *out_len  = dlen;
      return TRUE;
    }
  }
  return FALSE;
}

gboolean
pb_find_varint_field (const guint8 *buf, gsize len, guint32 want_field_num,
                      guint64 *out_value)
{
  gsize pos = 0;
  guint32 field_num;
  PbWireType wire_type;
  const guint8 *data;
  gsize dlen;
  guint64 vint;

  while (pb_read_field (buf, len, &pos, &field_num, &wire_type, &data, &dlen, &vint)) {
    if (field_num == want_field_num && wire_type == PB_WIRE_VARINT) {
      *out_value = vint;
      return TRUE;
    }
  }
  return FALSE;
}
