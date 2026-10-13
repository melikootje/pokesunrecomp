#include "romlib/blz.h"

namespace romlib {

Bytes blzDecompress(const Bytes &in) {
    const size_t size = in.size();
    if (size < 8)
        throw Error("blz: input too small");
    const uint32_t tb = rd32(&in[size - 8]);
    const uint32_t increment = rd32(&in[size - 4]);
    const size_t bottom = tb & 0xFFFFFF;
    const size_t top = tb >> 24;
    if (top < 8 || top > bottom || bottom > size)
        throw Error("blz: bad footer");
    if (increment > (1u << 28))
        throw Error("blz: implausible output size");

    Bytes out(in);
    out.resize(size + increment);
    size_t src = size - top; // exclusive end of the compressed stream, read backwards
    const size_t end = size - bottom;
    size_t dst = out.size();

    while (src > end) {
        uint8_t flags = out[--src];
        for (int i = 0; i < 8; i++) {
            if (src <= end)
                break;
            if (flags & 0x80) {
                if (src < end + 2)
                    throw Error("blz: truncated match");
                uint8_t hi = out[--src];
                uint8_t lo = out[--src];
                size_t len = size_t(hi >> 4) + 3;
                size_t disp = (size_t(hi & 0xF) << 8 | lo) + 3;
                if (len > dst - end || dst + disp > out.size())
                    throw Error("blz: bad match");
                for (size_t k = 0; k < len; k++) {
                    --dst;
                    out[dst] = out[dst + disp];
                }
            } else {
                if (dst <= end)
                    throw Error("blz: output overrun");
                out[--dst] = out[--src];
            }
            flags = uint8_t(flags << 1);
        }
    }
    if (dst != end)
        throw Error("blz: output size mismatch");
    // The stream is decoded in place; the footer bytes were overwritten by decoded output.
    return out;
}

} // namespace romlib
