/*
 * ym2610.cpp — YM2610 sound synthesis, through ymfm (ext/ymfm, BSD-3).
 *
 * audio.c keeps modelling the timers, status and IRQ the sound driver
 * syncs to; this file only turns register writes into samples. FM and
 * ADPCM-A/B come out in stereo, the SSG in mono, mixed here. ADPCM
 * samples are read from the V ROMs, which on the Neo Geo serve both
 * ADPCM-A and ADPCM-B.
 */
#include "ymfm_opn.h"
#include <stdint.h>
#include <stddef.h>

namespace {

class neogeo_ym : public ymfm::ymfm_interface {
public:
    neogeo_ym() : chip(*this) {}

    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override {
        if (type != ymfm::ACCESS_ADPCM_A && type != ymfm::ACCESS_ADPCM_B) return 0;
        for (int i = 0; i < nvrom; i++) {
            if (address < vsize[i]) return vrom[i][address];
            address -= (uint32_t)vsize[i];
        }
        return 0;
    }

    ymfm::ym2610 chip;
    const uint8_t *vrom[8] = {};
    size_t vsize[8] = {};
    int nvrom = 0;
};

neogeo_ym *s_chip;

}

extern "C" {

void ym2610_init(const uint8_t *const *vroms, const size_t *sizes, int n) {
    if (!s_chip) s_chip = new neogeo_ym();
    s_chip->nvrom = n > 8 ? 8 : n;
    for (int i = 0; i < s_chip->nvrom; i++) { s_chip->vrom[i] = vroms[i]; s_chip->vsize[i] = sizes[i]; }
    s_chip->chip.set_fidelity(ymfm::OPN_FIDELITY_MED);
    s_chip->chip.reset();
}

void ym2610_reset(void) {
    if (s_chip) s_chip->chip.reset();
}

void ym2610_write(int port, uint8_t v) {
    if (s_chip) s_chip->chip.write((uint32_t)port, v);
}

/* The ADPCM end flags (port 2 reads status 1). */
uint8_t ym2610_read(int port) {
    return s_chip ? s_chip->chip.read((uint32_t)port) : 0;
}

/* `n` stereo samples at the chip's rate (8 MHz / 144). */
void ym2610_generate(int16_t *out, size_t n) {
    ymfm::ym2610::output_data o;
    for (size_t i = 0; i < n; i++) {
        int32_t l = 0, r = 0;
        if (s_chip) {
            s_chip->chip.generate(&o);
            l = o.data[0] + o.data[2];
            r = o.data[1] + o.data[2];
        }
        out[i * 2] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
        out[i * 2 + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
    }
}

void ym2610_shutdown(void) {
    delete s_chip;
    s_chip = nullptr;
}

}
