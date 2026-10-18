/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compiler-generated aggregate copies can call these symbols when SIMD is
 * disabled. KernelPatch does not export the libc names. Keep these tiny
 * implementations usable before module initialization resolves any symbols. */
typedef __SIZE_TYPE__ wx_size_t;
void *memcpy(void *destination, const void *source, wx_size_t size)
{
    volatile unsigned char *out = destination;
    const volatile unsigned char *in = source;
    wx_size_t i;
    for (i = 0; i < size; ++i) out[i] = in[i];
    return destination;
}
void *memset(void *destination, int value, wx_size_t size)
{
    volatile unsigned char *out = destination;
    wx_size_t i;
    for (i = 0; i < size; ++i) out[i] = (unsigned char)value;
    return destination;
}
void *memmove(void *destination, const void *source, wx_size_t size)
{
    volatile unsigned char *out = destination;
    const volatile unsigned char *in = source;
    wx_size_t i;
    if ((unsigned long)destination < (unsigned long)source) {
        for (i = 0; i < size; ++i) out[i] = in[i];
    } else {
        for (i = size; i; --i) out[i - 1] = in[i - 1];
    }
    return destination;
}
