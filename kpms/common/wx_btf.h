/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WX_BTF_H
#define WX_BTF_H

/* Self-contained, read-only decoder for the running kernel's embedded BTF.
 * No kernel structs, libc, or out-of-line helper references are required.
 * Public helpers return zero on success, otherwise a negative Linux errno;
 * func_arity returns the parameter count. Parameter indexes are zero based.
 */
struct wx_btf {
    const unsigned char *data;
    const char *strings;
    const unsigned char **types;
    unsigned int string_len, type_len, count;
    void (*free)(void *);
};

#define WX_BTF_EINVAL (-22)
#define WX_BTF_ENOENT (-2)
#define WX_BTF_ENOMEM (-12)
#define WX_BTF_EPROTO (-71)
#define WX_BTF_EOPNOTSUPP (-95)
#define WX_BTF_MAX_BLOB (256UL * 1024 * 1024)
#define WX_BTF_MAX_DEPTH 32U

static inline unsigned int wx_btf_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | (unsigned int)p[1] << 8 |
        (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}
static inline int wx_btf_equal(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static inline const char *wx_btf_string(const struct wx_btf *b, unsigned int off)
{
    unsigned int i;
    if (off >= b->string_len) return (void *)0;
    for (i = off; i < b->string_len; ++i)
        if (!b->strings[i]) return b->strings + off;
    return (void *)0;
}
static inline unsigned int wx_btf_kind(const unsigned char *t)
{
    return (wx_btf_u32(t + 4) >> 24) & 31U;
}
static inline const unsigned char *wx_btf_type(const struct wx_btf *b, unsigned int id)
{
    return id && id <= b->count ? b->types[id] : (void *)0;
}
static inline unsigned int wx_btf_strip(const struct wx_btf *b, unsigned int id, int pointers)
{
    unsigned int depth;
    for (depth = 0; depth < WX_BTF_MAX_DEPTH; ++depth) {
        const unsigned char *t = wx_btf_type(b, id);
        unsigned int kind;
        if (!t) return 0;
        kind = wx_btf_kind(t);
        if (kind != 8 && kind != 9 && kind != 10 && kind != 11 && kind != 18 &&
            !(pointers && kind == 2)) return id;
        id = wx_btf_u32(t + 8);
    }
    return 0;
}
static inline int wx_btf_type_size(const struct wx_btf *b, unsigned int id,
                                    unsigned int *size, unsigned int depth)
{
    const unsigned char *t;
    unsigned int kind, element_size, count;
    if (depth >= WX_BTF_MAX_DEPTH || !(id = wx_btf_strip(b, id, 0))) return WX_BTF_EPROTO;
    t = wx_btf_type(b, id); kind = wx_btf_kind(t);
    if (kind == 2) { *size = 8; return 0; } /* ARM64 kernel pointers */
    if (kind == 1) {
        unsigned int encoding = wx_btf_u32(t + 12), bytes = wx_btf_u32(t + 8);
        /* Older BTF can encode bitfields in INT's offset/bits instead of the
         * enclosing STRUCT's kflag. Reject either representation. */
        if (!bytes || bytes > 16 || (encoding & 0x00ff0000U) ||
            (encoding & 255U) != bytes * 8) return WX_BTF_EPROTO;
        *size = bytes; return 0;
    }
    if (kind == 4 || kind == 5 || kind == 6 || kind == 16 || kind == 19) {
        *size = wx_btf_u32(t + 8); return 0;
    }
    if (kind != 3 || wx_btf_type_size(b, wx_btf_u32(t + 12), &element_size, depth + 1))
        return WX_BTF_EPROTO;
    count = wx_btf_u32(t + 20);
    if (element_size && count > (~0U) / element_size) return WX_BTF_EPROTO;
    *size = element_size * count;
    return 0;
}
static inline void wx_btf_destroy(struct wx_btf *b)
{
    if (b->types && b->free) b->free(b->types);
    b->data = (void *)0; b->strings = (void *)0; b->types = (void *)0;
    b->string_len = b->type_len = b->count = 0; b->free = (void *)0;
}
static inline int wx_btf_init(struct wx_btf *b, unsigned long (*lookup)(const char *),
                               void *(*alloc)(unsigned long, unsigned int),
                               void (*free_fn)(void *), unsigned int alloc_flags)
{
    unsigned long start, stop, total;
    const unsigned char *blob;
    unsigned int header, type_off, str_off, str_len, type_len, cursor, count = 0, pass;
    int error = WX_BTF_EPROTO;
    if (!b) return WX_BTF_EINVAL;
    b->data = (void *)0; b->strings = (void *)0; b->types = (void *)0;
    b->string_len = b->type_len = b->count = 0; b->free = free_fn;
    if (!lookup || !alloc || !free_fn || sizeof(unsigned int) != 4 || sizeof(void *) != 8)
        return WX_BTF_EINVAL;
    start = lookup("__start_BTF"); stop = lookup("__stop_BTF");
    if (!start || !stop) return WX_BTF_ENOENT;
    if (stop <= start || stop - start < 24 || stop - start > WX_BTF_MAX_BLOB)
        return WX_BTF_EPROTO;
    total = stop - start; blob = (const unsigned char *)start;
    if (blob[0] != 0x9f || blob[1] != 0xeb || blob[2] != 1 || blob[3]) return WX_BTF_EPROTO;
    header = wx_btf_u32(blob + 4); type_off = wx_btf_u32(blob + 8);
    type_len = wx_btf_u32(blob + 12); str_off = wx_btf_u32(blob + 16);
    str_len = wx_btf_u32(blob + 20);
    if (header < 24 || header > total || type_off > total - header ||
        type_len > total - header - type_off || str_off > total - header ||
        !str_len || str_len > total - header - str_off || !type_len ||
        (type_off < str_off + str_len && str_off < type_off + type_len)) return WX_BTF_EPROTO;
    b->data = blob + header + type_off; b->type_len = type_len;
    b->strings = (const char *)blob + header + str_off; b->string_len = str_len;
    if (b->strings[0] || b->strings[str_len - 1]) goto fail;
    /* Two passes: prove every record fits before allocating the ID index. */
    for (pass = 0; pass < 2; ++pass) {
        unsigned int id = 0;
        cursor = 0;
        while (cursor < type_len) {
            const unsigned char *t = b->data + cursor;
            unsigned int info, kind, vlen, extra = 0, unit = 0;
            if (type_len - cursor < 12) goto fail;
            info = wx_btf_u32(t + 4); kind = (info >> 24) & 31U; vlen = info & 65535U;
            if ((info & 0x60ff0000U) || !wx_btf_string(b, wx_btf_u32(t))) goto fail;
            switch (kind) {
            case 1: case 14: case 17: extra = 4; break;
            case 3: extra = 12; break;
            case 4: case 5: case 15: case 19: unit = 12; break;
            case 6: case 13: unit = 8; break;
            case 2: case 7: case 8: case 9: case 10: case 11:
            case 12: case 16: case 18: break;
            default: error = WX_BTF_EOPNOTSUPP; goto fail;
            }
            extra += unit * vlen;
            if (extra > type_len - cursor - 12) goto fail;
            if (pass) b->types[++id] = t; else ++count;
            cursor += 12 + extra;
        }
        if (!pass) {
            b->count = count;
            b->types = alloc(((unsigned long)count + 1) * sizeof(*b->types), alloc_flags);
            if (!b->types) { error = WX_BTF_ENOMEM; goto fail; }
            b->types[0] = (void *)0;
        }
    }
    return 0;
fail:
    wx_btf_destroy(b);
    return error;
}
/* Prefer concrete named aggregates; typedefs cover anonymous atomic_t, etc.
 * Ambiguous definitions fail closed instead of choosing a layout by chance. */
static inline int wx_btf_named(const struct wx_btf *b, const char *name, unsigned int *id)
{
    unsigned int i, pass, found;
    if (!b || !b->types || !name || !*name || !id) return WX_BTF_EINVAL;
    for (pass = 0; pass < 2; ++pass) {
        found = 0;
        for (i = 1; i <= b->count; ++i) {
            const unsigned char *t = b->types[i];
            unsigned int kind = wx_btf_kind(t), resolved;
            const char *n;
            if (pass ? kind != 8 : (kind != 4 && kind != 5)) continue;
            n = wx_btf_string(b, wx_btf_u32(t));
            if (!n || !wx_btf_equal(n, name)) continue;
            resolved = wx_btf_strip(b, i, 0);
            if (!resolved || (found && found != resolved)) return WX_BTF_EPROTO;
            found = resolved;
        }
        if (found) { *id = found; return 0; }
    }
    return WX_BTF_ENOENT;
}
static inline int wx_btf_size(const struct wx_btf *b, const char *name, unsigned int *size)
{
    unsigned int id;
    int rc;
    if (!size) return WX_BTF_EINVAL;
    rc = wx_btf_named(b, name, &id);
    return rc ? rc : wx_btf_type_size(b, id, size, 0);
}
static inline int wx_btf_member_id(const struct wx_btf *b, unsigned int id,
                                  const char *name, unsigned int bytes,
                                  unsigned int *offset, unsigned int depth)
{
    const unsigned char *t;
    unsigned int i, info, kind, total, result = 0, found = 0;
    if (depth >= WX_BTF_MAX_DEPTH || !(id = wx_btf_strip(b, id, 0))) return WX_BTF_EPROTO;
    t = wx_btf_type(b, id); info = wx_btf_u32(t + 4); kind = wx_btf_kind(t);
    if (kind != 4 && kind != 5) return WX_BTF_EPROTO;
    total = wx_btf_u32(t + 8);
    for (i = 0; i < (info & 65535U); ++i) {
        const unsigned char *m = t + 12 + i * 12;
        const char *member_name = wx_btf_string(b, wx_btf_u32(m));
        unsigned int bit_offset = wx_btf_u32(m + 8), member_id = wx_btf_u32(m + 4);
        unsigned int field_bits = (info >> 31) ? bit_offset >> 24 : 0;
        unsigned int field_size, nested_offset = 0, at;
        int rc;
        if (!member_name) return WX_BTF_EPROTO;
        if (info >> 31) bit_offset &= 0xffffffU;
        if (*member_name && !wx_btf_equal(member_name, name)) continue;
        if (field_bits || (bit_offset & 7)) return WX_BTF_EPROTO;
        at = bit_offset / 8;
        rc = wx_btf_type_size(b, member_id, &field_size, 0);
        if (rc || at > total || field_size > total - at) return WX_BTF_EPROTO;
        if (!*member_name) {
            unsigned int nested_id = wx_btf_strip(b, member_id, 0);
            const unsigned char *nested = wx_btf_type(b, nested_id);
            if (!nested || (wx_btf_kind(nested) != 4 && wx_btf_kind(nested) != 5)) continue;
            rc = wx_btf_member_id(b, nested_id, name, bytes, &nested_offset, depth + 1);
            if (rc == WX_BTF_ENOENT) continue;
            if (rc) return rc;
        } else if (field_size != bytes) return WX_BTF_EPROTO;
        if (nested_offset > field_size || bytes > field_size - nested_offset ||
            at > total || nested_offset > total - at || bytes > total - at - nested_offset || found)
            return WX_BTF_EPROTO;
        result = at + nested_offset; found = 1;
    }
    if (!found) return WX_BTF_ENOENT;
    *offset = result;
    return 0;
}
static inline int wx_btf_member(const struct wx_btf *b, const char *struct_name,
                               const char *member_name, unsigned int expected_bytes,
                               unsigned int *offset)
{
    unsigned int id;
    int rc;
    if (!member_name || !*member_name || !expected_bytes || !offset) return WX_BTF_EINVAL;
    rc = wx_btf_named(b, struct_name, &id);
    return rc ? rc : wx_btf_member_id(b, id, member_name, expected_bytes, offset, 0);
}
static inline int wx_btf_function(const struct wx_btf *b, const char *name,
                                 const unsigned char **prototype)
{
    unsigned int i, found = 0;
    if (!b || !b->types || !name || !*name || !prototype) return WX_BTF_EINVAL;
    for (i = 1; i <= b->count; ++i) {
        const unsigned char *t = b->types[i];
        const char *n;
        unsigned int id;
        if (wx_btf_kind(t) != 12) continue;
        n = wx_btf_string(b, wx_btf_u32(t));
        if (!n || !wx_btf_equal(n, name)) continue;
        id = wx_btf_u32(t + 8); t = wx_btf_type(b, id);
        if (!t || wx_btf_kind(t) != 13 || (found && found != id)) return WX_BTF_EPROTO;
        found = id;
    }
    if (!found) return WX_BTF_ENOENT;
    *prototype = b->types[found];
    return 0;
}
static inline int wx_btf_func_arity(const struct wx_btf *b, const char *name)
{
    const unsigned char *proto;
    int rc = wx_btf_function(b, name, &proto);
    return rc ? rc : (int)(wx_btf_u32(proto + 4) & 65535U);
}
/* Some non-inlined kallsyms functions lack a BTF FUNC record. Use this only
 * for an independently audited, version-bounded ABI, and separately require
 * the exact kallsyms symbol to exist. A present incompatible record fails. */
static inline int wx_btf_optional_arity(const struct wx_btf *b, const char *name,
                                       unsigned int expected)
{
    int actual = wx_btf_func_arity(b, name);
    if (actual == WX_BTF_ENOENT) return 0;
    return actual < 0 ? actual : (unsigned int)actual == expected ? 0 : WX_BTF_EPROTO;
}
static inline int wx_btf_func_param_named(const struct wx_btf *b, const char *function,
                                         unsigned int index, const char *name)
{
    const unsigned char *proto, *type;
    const char *actual;
    unsigned int id;
    int rc;
    if (!name || !*name) return WX_BTF_EINVAL;
    rc = wx_btf_function(b, function, &proto);
    if (rc) return rc;
    if (index >= (wx_btf_u32(proto + 4) & 65535U)) return WX_BTF_EINVAL;
    id = wx_btf_strip(b, wx_btf_u32(proto + 12 + index * 8 + 4), 1);
    type = wx_btf_type(b, id);
    if (!type) return WX_BTF_EPROTO;
    actual = wx_btf_string(b, wx_btf_u32(type));
    return actual && wx_btf_equal(actual, name) ? 0 : WX_BTF_EPROTO;
}
#endif
