/*
 * libapplegfx-vulkan — ring-wrap header read unit test
 * tests/ring-wrap-header-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * A command header (or body) that starts near the end of a ring page
 * straddles two ring pages whose backing physical pages are NOT
 * contiguous. lagfx_ring_read_bytes must resolve each 4 KiB chunk
 * through the PFN-array; a flat read pulls the tail from the wrong
 * physical page. This pins that behavior: a 12-byte header at ring
 * offset 0xffc must read its length/stamp dwords from the SECOND
 * (non-adjacent) physical page. Revert ring_common's chunking and
 * this test fails.
 */

#include "dispatchers/ring_common.h"
#include "protocol/state.h"
#include "device.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RING_SIZE   0x2000u   /* two ring pages */
#define PAGE0_PFN   0x100u    /* PFN-array page */
#define DATA_PFN_A  0x200u    /* ring page 0 backing (arbitrary) */
#define DATA_PFN_B  0x900u    /* ring page 1 backing — deliberately NOT 0x201 */

/* Fake guest memory: the PFN-array page maps ring page i -> DATA_PFN_A/B,
 * and each data page carries a recognizable fill so a mis-resolved read is
 * detectable. The header we care about lives at ring offset 0xffc: its
 * first 4 bytes are in page 0 (DATA_PFN_A), the remaining 8 in page 1
 * (DATA_PFN_B). We plant opcode=0x0007 length=0x40 stamp=0xdeadbeef there. */
static bool fake_read(void *opaque, uint64_t gpa, uint64_t len, void *dst) {
    (void)opaque;
    uint8_t *out = (uint8_t *)dst;
    uint64_t page = gpa >> 12;
    uint64_t off  = gpa & 0xfffu;

    if (page == PAGE0_PFN) {          /* PFN-array: u32 per ring page */
        uint32_t idx = (uint32_t)(off / 4u);
        uint32_t pfn = (idx == 0u) ? DATA_PFN_A : DATA_PFN_B;
        assert(len == 4u);
        memcpy(out, &pfn, 4);
        return true;
    }

    /* Data pages: fill with the page's own PFN byte, then overlay the
     * planted header split across the 0xffc boundary. */
    uint8_t fillA = 0xAA, fillB = 0xBB;
    for (uint64_t i = 0; i < len; i++)
        out[i] = (page == DATA_PFN_A) ? fillA : fillB;

    /* Header bytes: opcode(u16)=0x0007, arg(u16)=0, length(u32)=0x40,
     * stamp(u32)=0xdeadbeef. offsets 0..11 within the command. The command
     * starts at ring off 0xffc → bytes 0..3 land at page0 off 0xffc..0xfff,
     * bytes 4..11 at page1 off 0..7. */
    static const uint8_t HDR[12] = {
        0x07, 0x00, 0x00, 0x00,          /* opcode=7, arg=0 */
        0x40, 0x00, 0x00, 0x00,          /* length=0x40 */
        0xef, 0xbe, 0xad, 0xde,          /* stamp=0xdeadbeef */
    };
    if (page == DATA_PFN_A) {
        /* command bytes 0..3 at page off 0xffc..0xfff */
        for (uint64_t i = 0; i < len; i++) {
            uint64_t poff = off + i;
            if (poff >= 0xffcu && poff < 0x1000u)
                out[i] = HDR[poff - 0xffcu];
        }
    } else if (page == DATA_PFN_B) {
        /* command bytes 4..11 at page off 0..7 */
        for (uint64_t i = 0; i < len; i++) {
            uint64_t poff = off + i;
            if (poff < 8u)
                out[i] = HDR[4u + poff];
        }
    }
    return true;
}

int main(void) {
    lagfx_device_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.desc.shell.read_memory = fake_read;

    lagfx_protocol_t p;
    memset(&p, 0, sizeof(p));
    p.dev = &dev;

    uint64_t page0_gpa = (uint64_t)PAGE0_PFN << 12;

    /* Resolve the byte at ring offset 0xffc → must land in DATA_PFN_A. */
    uint64_t g0 = 0;
    assert(lagfx_ring_resolve_data_gpa(&p, page0_gpa, RING_SIZE, 0xffcu, &g0));
    assert((g0 >> 12) == DATA_PFN_A);
    /* Resolve ring offset 0x1000 → the NEXT ring page → DATA_PFN_B. */
    uint64_t g1 = 0;
    assert(lagfx_ring_resolve_data_gpa(&p, page0_gpa, RING_SIZE, 0x1000u, &g1));
    assert((g1 >> 12) == DATA_PFN_B);
    assert((g1 >> 12) != (g0 >> 12) + 1u);  /* backing pages non-contiguous */

    /* Read the 12-byte header straddling the boundary. */
    uint8_t hdr[12];
    memset(hdr, 0, sizeof(hdr));
    bool ok = lagfx_ring_read_bytes(&p, page0_gpa, RING_SIZE, 0xffcu, 12u, hdr);
    assert(ok);

    uint16_t opcode = (uint16_t)(hdr[0] | (hdr[1] << 8));
    uint32_t length = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8)
                    | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    uint32_t stamp  = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8)
                    | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);

    /* The whole point: length/stamp come from the SECOND physical page.
     * A flat read would have pulled fillA (0xAA) here. */
    assert(opcode == 0x0007u);
    assert(length == 0x40u);
    assert(stamp  == 0xdeadbeefu);

    printf("ring-wrap-header-unit: OK (opcode=0x%04x length=0x%x stamp=0x%08x across non-contiguous pages)\n",
           opcode, length, stamp);
    return 0;
}
