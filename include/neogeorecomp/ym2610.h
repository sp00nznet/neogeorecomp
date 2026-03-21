/*
 * ym2610.h — Yamaha YM2610 (OPNB) sound chip interface.
 *
 * The YM2610 provides 14 audio channels across three synthesis engines:
 *
 *   FM (Frequency Modulation):  4 channels, each with 4 operators
 *   SSG (Software Sound Gen):   3 channels, square/noise waveforms
 *   ADPCM-A:                    6 channels, short samples (SFX)
 *   ADPCM-B:                    1 channel, streaming samples (music/voice)
 *
 * The Z80 audio CPU controls the YM2610 via I/O ports:
 *   Port $04/$05: Address/Data pair A (SSG, ADPCM-B, FM ch1-2)
 *   Port $06/$07: Address/Data pair B (ADPCM-A, FM ch3-4)
 *
 * Audio sample data comes from the V ROMs.
 *
 * For the recompiler runtime, the YM2610 can be emulated using:
 *   - The ymfm library (accurate cycle-level emulation)
 *   - A simplified HLE approach (register capture + sample playback)
 */

#ifndef NEOGEORECOMP_YM2610_H
#define NEOGEORECOMP_YM2610_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Initialization ----- */

int ym2610_init(int sample_rate);
void ym2610_shutdown(void);

/* ----- V ROM Loading ----- */

/*
 * Load ADPCM sample data from V ROM files.
 * The V ROMs contain both ADPCM-A (short samples) and ADPCM-B
 * (streaming) data. On newer boards with the NEO-PCM2 chip,
 * the data is interleaved; on older boards, separate ROMs are used.
 */
int ym2610_load_vrom(const char **vrom_paths, int num_vroms);

/* ----- Register Access (called by Z80 layer) ----- */

void ym2610_write(uint8_t port, uint8_t addr, uint8_t data);
uint8_t ym2610_read(uint8_t port);

/* ----- Audio Generation ----- */

/*
 * Generate audio samples into the output buffer.
 *
 * Called by the platform layer to fill the audio output buffer.
 * Generates stereo interleaved 16-bit samples.
 *
 * buffer: output buffer (stereo interleaved, left then right)
 * num_samples: number of stereo sample pairs to generate
 */
void ym2610_generate(int16_t *buffer, int num_samples);

/* ----- Timer ----- */

/*
 * The YM2610 has two internal timers (Timer A and Timer B) that
 * generate IRQs to the Z80. The runtime must tick these at the
 * correct rate.
 */
void ym2610_tick_timers(int cycles);

/* Check if the YM2610 is requesting an IRQ. */
int ym2610_irq_pending(void);

/* ----- Reset ----- */

void ym2610_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NEOGEORECOMP_YM2610_H */
