#ifndef __GE_ADDRS_HH__
#define __GE_ADDRS_HH__

#include <cstdint>

/* GoldenEye 007 (NGEE, NTSC-U) -- every address here was recovered from the ROM:
 * libaudio entry points by matching the audio init (0x80006a2c..) against Perfect
 * Dark's sndInit and by the event-type constant each seqp API call posts.
 *
 * ROM layout: boot code is raw at ROM 0x1000 -> 0x80000400.  At ROM 0x21990 sits a
 * Rare "1172" (raw deflate) block the boot code inflates in place at 0x80020d90:
 * rspboot text, the RSP microcodes and all initialised data live inside it. */

#define GE_ROM_BOOT_OFFS     0x1000u
#define GE_BOOT_VADDR        0x80000400u
#define GE_ROM_DATA_1172     0x21990u
#define GE_DATA_VADDR        0x80020d90u

/* libaudio entry points we call */
#define GE_alHeapInit        0x80010d30u  /* (ALHeap*, u8 *base, s32 len)            */
#define GE_alInit            0x8000eb1cu  /* (ALGlobals*, ALSynConfig*)              */
#define GE_alBnkfNew         0x80010e74u  /* (ALBankFile*, u8 *tbl)                  */
#define GE_alCSPNew          0x80012080u  /* (ALCSPlayer*, ALSeqpConfig*)            */
#define GE_alCSPSetBank      0x800121f0u  /* (ALCSPlayer*, ALBank*)                  */
#define GE_alCSeqNew         0x8001279cu  /* (ALCSeq*, u8 *data)                     */
#define GE_alCSPSetSeq       0x80012d00u  /* (ALCSPlayer*, ALCSeq*)                  */
#define GE_alCSPPlay         0x80012d40u  /* (ALCSPlayer*)                           */
#define GE_alCSPSetVol       0x80012da0u  /* (ALCSPlayer*, s16 vol)                  */
#define GE_alAudioFrame      0x8000f108u  /* (Acmd*, s32 *cmdLen, s16 *out, s32 n)   */

/* data inside the inflated segment */
#define GE_rspbootText       0x80020d90u  /* OSTask.ucode_boot                       */
#define GE_rspbootText_LEN   0xd0u
#define GE_aspMainText       0x80022280u  /* audio microcode text (OSTask.ucode)     */
#define GE_aspMainData       0x8005d020u  /* audio microcode data, 0x800 bytes       */
#define GE_custom_fx_params  0x80023100u  /* s32[50] custom reverb table (fxType 6)  */
#define GE_music_volume      0x80024338u  /* u16 master music volume (0x7fff)        */
#define GE_track_volumes     0x80024358u  /* s16[] per-sequence volume, Q15          */

/* audio assets in ROM */
#define GE_ROM_INST_CTL      0x3b4450u
#define GE_ROM_INST_CTL_LEN  0x43a0u
#define GE_ROM_INST_TBL      0x3b87f0u
#define GE_ROM_INST_TBL_LEN  0x60fa0u
#define GE_ROM_SEQ_TABLE     0x419790u

/* synth config as GoldenEye sets it (0x80006cbc..): 24 physical voices, 128 updates,
 * fxType 6 (AL_FX_CUSTOM); osAiSetFrequency(22050) -> dacRate 2208 -> 22047 Hz. */
#define GE_MAX_PVOICES       24
#define GE_MAX_UPDATES       128
#define GE_FX_TYPE           6
#define GE_OUTPUT_RATE       22047
#define GE_FRAME_SAMPLES     736          /* ceil16(2*rate/60): amgr's nominal frame  */
#define GE_SEQP_MAX_VOICES   16
#define GE_SEQP_MAX_EVENTS   64
#define GE_SEQP_MAX_CHANNELS 16

#endif
