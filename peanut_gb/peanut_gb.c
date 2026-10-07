#include <nuttx/config.h>

#include <mcp/peanut_gb.h>

#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <nuttx/input/keyboard.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <nuttx/audio/audio.h>
#include <mqueue.h>

#ifdef CONFIG_MCP_APPS_PEANUT_GB_SOUND
#include "Peanut-GB/examples/sdl2/minigb_apu/minigb_apu.h"
#define ENABLE_SOUND 1
static uint8_t audio_read(uint16_t addr);
static void audio_write(uint16_t addr, uint8_t val);
#else
#define ENABLE_SOUND 0
#endif /*CONFIG_MCP_APPS_PEANUT_GB_SOUND*/

#define ENABLE_LCD 1
#include "Peanut-GB/peanut_gb.h"

#define SHARD_COUNT 16

#define MAX_ROM_SIZE (1024 * 1024)
#define SHARD_SIZE (MAX_ROM_SIZE / SHARD_COUNT)
#define SCRW 160
#define SCRH 144
#define GB_TIMER_PERIOD (1000.0 / VERTICAL_SYNC + 1.0)

#define PTF(shuffle_flags, triplet_id) (((shuffle_flags) << 5) | (triplet_id))
// extend g to 6 bits using linear scaling. Bit replication would produce
// the same result in this case (for 5->6 bits).
#define FIVEBITRGB_TO_RGB565(r, g, b) ((r << 11) | (((g * 63 + 15) / 31) << 5) | b)
#define OBP0 0
#define OBP1 1
#define BGP  2

#ifdef CONFIG_MCP_APPS_PEANUT_GB_SOUND

#define AUD_MQ_NAME "pgb_aud"

typedef struct {
    bool in_use;
    struct ap_buffer_s * apbuf;
} audio_driver_buf_t;

#endif /*CONFIG_MCP_APPS_PEANUT_GB_SOUND*/

typedef struct {
    lv_obj_t * base_obj;
    char * save_path;
    lv_timer_t * timer;
    uint8_t * cart_ram;
    uint32_t first_tick;
    uint32_t frame_count;
    lv_obj_t * canv;
    int keypad_fd;
    uint8_t size_val;
    bool screenbuf_dirty;
    int8_t palette_number;
    uint8_t screen_joypad_mask;
    uint8_t uinput_joypad_mask;
    bool style_is_init;
    lv_style_t btn_style;
    struct gb_s gb;
    uint8_t * rom_shards[SHARD_COUNT];
#ifdef CONFIG_MCP_APPS_PEANUT_GB_SOUND
    bool aud_working;
    int aud_fd;
    mqd_t aud_mq;
    apb_samp_t adb_act;
    apb_samp_t adb_rem;
    apb_samp_t adb_pos;
    apb_samp_t n_audio_driver_bufs;
    size_t apu_rem;
    size_t apu_pos;
    audio_driver_buf_t * audio_driver_bufs;
    struct minigb_apu_ctx apu;
    int16_t apu_sample_buf[AUDIO_SAMPLES_TOTAL];
#endif
    bool dirty_rows[SCRH];
    uint16_t palette[0x23 + 1];
    uint8_t raw_buf[SCRH][SCRW];
    uint16_t canv_buf[SCRH][SCRW];
} ctx_t;

static uint8_t gb_rom_read(struct gb_s * gb, const uint_fast32_t addr);
static void open_menu(ctx_t * ctx);

#ifdef CONFIG_MCP_APPS_PEANUT_GB_SOUND

static struct minigb_apu_ctx * g_apu_p;

static uint8_t audio_read(uint16_t addr)
{
    return minigb_apu_audio_read(g_apu_p, addr);
}

static void audio_write(uint16_t addr, uint8_t val)
{
    minigb_apu_audio_write(g_apu_p, addr, val);
}

static void setup_sound(ctx_t * ctx)
{
    int res;
    struct audio_caps_s cap;
    struct audio_buf_desc_s buf_desc;

    DIR * dirp = NULL;
    ctx->aud_fd = -1;
    bool device_reserved = false;
    ctx->aud_mq = (mqd_t) -1;
    bool mq_registered = false;
    bool started = false;

    minigb_apu_audio_init(&ctx->apu);
    g_apu_p = &ctx->apu;

    dirp = opendir("/dev/audio");
    if(!dirp) {
        fprintf(stderr, "no \"/dev/audio\" directory\n");
        goto early_out;
    }

    while(1) {
        struct dirent * dirent = readdir(dirp);
        if(!dirent) {
            fprintf(stderr, "no suitable audio device in \"/dev/audio\"\n");
            goto early_out;
        }

        char * path;
        res = asprintf(&path, "/dev/audio/%s", dirent->d_name);
        assert(res >= 0);
        if(ctx->aud_fd >= 0) close(ctx->aud_fd);
        ctx->aud_fd = open(path, O_RDWR);
        free(path);
        if(ctx->aud_fd < 0) {
            continue;
        }

        memset(&cap, 0, sizeof(cap));
        cap.ac_len = sizeof(cap);
        cap.ac_type = AUDIO_TYPE_QUERY;
        cap.ac_subtype = AUDIO_TYPE_QUERY;
        res = ioctl(ctx->aud_fd, AUDIOIOC_GETCAPS, (unsigned long)&cap);
        if(res != sizeof(cap)) {
            continue;
        }

        if(false == (
                     ((cap.ac_format.hw & (1 << (AUDIO_FMT_PCM - 1))) != 0) &&
                     (cap.ac_controls.b[0] & AUDIO_TYPE_OUTPUT)
                    )) {
            continue;
        }

        memset(&cap, 0, sizeof(cap));
        cap.ac_len = sizeof(cap);
        cap.ac_type = AUDIO_TYPE_OUTPUT;
        cap.ac_subtype = AUDIO_TYPE_QUERY;
        res = ioctl(ctx->aud_fd, AUDIOIOC_GETCAPS, (unsigned long)&cap);
        if(res != sizeof(cap)) {
            continue;
        }

        uint8_t minimum_channels = cap.ac_channels >> 4;
        if(minimum_channels && minimum_channels > 1) {
            continue;
        }

        break;
    }

    res = ioctl(ctx->aud_fd, AUDIOIOC_RESERVE, 0);
    if(res < 0) {
        fprintf(stderr, "failed to reserve the audio device\n");
        goto early_out;
    }
    device_reserved = true;

    struct audio_caps_desc_s cap_desc = {0};
    cap_desc.caps.ac_len            = sizeof(cap_desc.caps);
    cap_desc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
    cap_desc.caps.ac_channels       = 1;
    cap_desc.caps.ac_chmap          = 0;
    cap_desc.caps.ac_controls.hw[0] = (uint32_t)AUDIO_SAMPLE_RATE;
    cap_desc.caps.ac_controls.b[3]  = (uint32_t)AUDIO_SAMPLE_RATE >> 16;
    cap_desc.caps.ac_controls.b[2]  = 16;
    cap_desc.caps.ac_subtype        = AUDIO_FMT_PCM;
    res = ioctl(ctx->aud_fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc);
    if(res != 0) {
        fprintf(stderr, "failed to configure the audio device\n");
        goto early_out;
    }

    struct ap_buffer_info_s buf_info;
    res = ioctl(ctx->aud_fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&buf_info);
    if(res != 0) {
        buf_info.nbuffers = 2;
        buf_info.buffer_size = 8192;
    }
    ctx->n_audio_driver_bufs = buf_info.nbuffers;

    struct mq_attr attr = {
        .mq_maxmsg = buf_info.nbuffers + 8,
        .mq_msgsize = sizeof(struct audio_msg_s),
    };
    /* ideally this would have O_NONBLOCK but drivers count on it being blocking */
    ctx->aud_mq = mq_open(AUD_MQ_NAME, O_RDWR | O_CREAT | O_EXCL, 0644, &attr);
    if(ctx->aud_mq == (mqd_t) -1) {
        fprintf(stderr, "could not create an audio message queue\n");
        goto early_out;
    }

    res = ioctl(ctx->aud_fd, AUDIOIOC_REGISTERMQ, (unsigned long)ctx->aud_mq);
    if(res != 0) {
        fprintf(stderr, "could not register the audio message queue\n");
        goto early_out;
    }
    mq_registered = true;

    ctx->audio_driver_bufs = calloc(buf_info.nbuffers, sizeof(*ctx->audio_driver_bufs));
    assert(ctx->audio_driver_bufs);

    for(apb_samp_t i = 0; i < buf_info.nbuffers; i++) {
        memset(&buf_desc, 0, sizeof(buf_desc));
        buf_desc.numbytes = buf_info.buffer_size;
        buf_desc.u.pbuffer = &ctx->audio_driver_bufs[i].apbuf;
        res = ioctl(ctx->aud_fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc);
        if(res != sizeof(buf_desc)) {
            fprintf(stderr, "audio driver could not allocate a buffer\n");
            ctx->audio_driver_bufs[i].apbuf = NULL;
            goto early_out;
        }
    }

    res = ioctl(ctx->aud_fd, AUDIOIOC_START, 0);
    if(res != 0) {
        fprintf(stderr, "could not start the audio device\n");
        goto early_out;
    }
    started = true;

    ctx->aud_working = true;

early_out:
    if(ctx->aud_working == false) {
        if(started) ioctl(ctx->aud_fd, AUDIOIOC_STOP, 0);
        if(ctx->audio_driver_bufs) {
            for(apb_samp_t i = 0; i < buf_info.nbuffers; i++) {
                struct ap_buffer_s * apb = ctx->audio_driver_bufs[i].apbuf;
                if(!apb) break;
                memset(&buf_desc, 0, sizeof(buf_desc));
                buf_desc.u.buffer = apb;
                ioctl(ctx->aud_fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
            }
            free(ctx->audio_driver_bufs);
        }
        if(mq_registered) ioctl(ctx->aud_fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)ctx->aud_mq);
        if(device_reserved) ioctl(ctx->aud_fd, AUDIOIOC_RELEASE, 0);
        if(ctx->aud_fd >= 0) close(ctx->aud_fd);
        if(ctx->aud_mq != (mqd_t) -1) {
            mq_close(ctx->aud_mq);
            res = mq_unlink(AUD_MQ_NAME);
            assert(res == 0);
        }
    }

    if(dirp) closedir(dirp);
}

static void teardown_sound(ctx_t * ctx)
{
    int res;

    if(!ctx->aud_working) return;

    ioctl(ctx->aud_fd, AUDIOIOC_STOP, 0);
    for(apb_samp_t i = 0; i < ctx->n_audio_driver_bufs; i++) {
        struct ap_buffer_s * apb = ctx->audio_driver_bufs[i].apbuf;
        if(!apb) break;
        struct audio_buf_desc_s buf_desc = {
            .u.buffer = apb,
        };
        ioctl(ctx->aud_fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
    }
    free(ctx->audio_driver_bufs);
    ioctl(ctx->aud_fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)ctx->aud_mq);
    ioctl(ctx->aud_fd, AUDIOIOC_RELEASE, 0);
    close(ctx->aud_fd);
    mq_close(ctx->aud_mq);
    res = mq_unlink(AUD_MQ_NAME);
    assert(res == 0);
}

static void flush_sound(ctx_t * ctx)
{
    int res;
    ssize_t rwres;
    struct mq_attr attr;

    if(!ctx->aud_working) return;

    res = mq_getattr(ctx->aud_mq, &attr);
    assert(res == 0);
    for(long m = 0; m < attr.mq_curmsgs; m++) {
        struct audio_msg_s msg;
        rwres = mq_receive(ctx->aud_mq, (char *) &msg, sizeof(msg), NULL);
        assert(rwres == sizeof(msg));
        if(msg.msg_id != AUDIO_MSG_DEQUEUE) continue;
        apb_samp_t i;
        for(i = 0; i < ctx->n_audio_driver_bufs; i++) {
            audio_driver_buf_t * adb = &ctx->audio_driver_bufs[i];
            if(adb->apbuf == msg.u.ptr) {
                assert(adb->in_use);
                adb->in_use = false;
                break;
            }
        }
        assert(i < ctx->n_audio_driver_bufs);
    }

    while(ctx->apu_rem) {
        if(ctx->adb_rem) {
            audio_driver_buf_t * adb = &ctx->audio_driver_bufs[ctx->adb_act];
            struct ap_buffer_s * apb = adb->apbuf;
            int16_t * dst = (int16_t *) apb->samp;

            while(ctx->apu_rem && ctx->adb_rem) {
                dst[ctx->adb_pos++] = (ctx->apu_sample_buf[ctx->apu_pos]
                                       + ctx->apu_sample_buf[ctx->apu_pos + 1])
                                      / 2;
                ctx->adb_rem--;
                ctx->apu_pos += 2;
                ctx->apu_rem -= 2;
            }

            if(ctx->adb_rem == 0) {
                struct audio_buf_desc_s buf_desc = {
                    .numbytes = apb->nbytes,
                    .u.buffer = apb,
                };
                res = ioctl(ctx->aud_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
                assert(res >= 0);
                adb->in_use = true;
            }

            continue;
        }

        apb_samp_t i;
        for(i = 0; i < ctx->n_audio_driver_bufs; i++) {
            audio_driver_buf_t * adb = &ctx->audio_driver_bufs[i];
            if(adb->in_use == false) {
                struct ap_buffer_s * apb = adb->apbuf;
                assert(apb->nmaxbytes > 0);
                assert(apb->nmaxbytes % 2 == 0);
                assert((uintptr_t)apb->samp % 2 == 0);

                ctx->adb_act = i;
                ctx->adb_rem = apb->nmaxbytes / 2;
                ctx->adb_pos = 0;

                apb->curbyte = 0;
                apb->flags = 0;
                apb->nbytes = apb->nmaxbytes;

                break;
            }
        }

        if(!(i < ctx->n_audio_driver_bufs)) {
            return;
        }
    }
}

#endif /*CONFIG_MCP_APPS_PEANUT_GB_SOUND*/

// All the palette code is from the Game Boy Color boot ROM.
// In particular, the disassembly at
// https://codeberg.org/ISSOtm/gb-bootroms/src/commit/e10154ee7962873d9fa7ec02be6a6d4299bad208/src/cgb.asm

static uint8_t title2index(const uint8_t title[16])
{
    uint8_t sum = 0;

    for(int i = 0; i < 16; i++) {
        sum += title[i];
    }

    static const uint8_t title_checksums[] = {
        0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C,
        0x8C, 0x92, 0x3D, 0x5C, 0x58, 0xC9, 0x3E, 0x70,
        0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
        0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97,
        0x4B, 0x90, 0x17, 0x10, 0x39, 0xF7, 0xF6, 0xA2,
        0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
        0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01,
        0x9D, 0x71, 0x9C, 0xBD, 0x5D, 0x6D, 0x67, 0x3F,
        0x6B,
        // These 14 ambiguous checksums are also discriminated based on the 4th title letter
        0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27, 0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4,
    };

    const uint8_t * title_checksum_ptr = memchr(
        title_checksums, sum, sizeof(title_checksums));

    if(title_checksum_ptr == NULL) {
        return 0;
    }

    uint8_t index = title_checksum_ptr - title_checksums;

    if(index < 65) {
        return index;
    }

    static const uint8_t fourth_letter_table[14+14+1] =
        "BEFAARBEKEK R-"
        "URAR INAILICE "
        "R"
    ;

    uint8_t fourth_letter = title[3];
    uint8_t fourth_letters_index = index - 65;

    do {
        if(fourth_letter == fourth_letter_table[fourth_letters_index]) {
            return index;
        }

        index += 14;
        fourth_letters_index += 14;

    } while(fourth_letters_index < sizeof(fourth_letter_table));

    return 0;
}

static uint8_t title2ptf(const uint8_t title[16])
{
    uint8_t index = title2index(title);

    static const uint8_t pal_triplet_ids_and_flags[] = {
        PTF(0b011,28), PTF(0b000, 8), PTF(0b000,18), PTF(0b101, 3), PTF(0b101, 2), PTF(0b000, 7), PTF(0b100, 7), PTF(0b010,11),
        PTF(0b001, 0), PTF(0b000,18), PTF(0b011, 5), PTF(0b101, 8), PTF(0b000,22), PTF(0b101, 9), PTF(0b100, 6), PTF(0b101,17),
        PTF(0b011, 8), PTF(0b101, 0), PTF(0b100, 7), PTF(0b011, 6), PTF(0b000,18), PTF(0b101, 1), PTF(0b001,16), PTF(0b001,28),
        PTF(0b000,18), PTF(0b100, 5), PTF(0b000,18), PTF(0b011, 4), PTF(0b000,27), PTF(0b000, 7), PTF(0b000, 6), PTF(0b011,15),
        PTF(0b011,14), PTF(0b011,14), PTF(0b101,14), PTF(0b101,15), PTF(0b011,15), PTF(0b101,18), PTF(0b101,15), PTF(0b101,18),
        PTF(0b101, 8), PTF(0b101,11), PTF(0b011,15), PTF(0b101,15), PTF(0b100, 6), PTF(0b101,14), PTF(0b101, 2), PTF(0b101, 2),
        PTF(0b000,18), PTF(0b101,15), PTF(0b000,19), PTF(0b000,18), PTF(0b101, 1), PTF(0b011,14), PTF(0b101,15), PTF(0b101,15),
        PTF(0b101,13), PTF(0b000, 6), PTF(0b010,12), PTF(0b011,14), PTF(0b101,15), PTF(0b101,15), PTF(0b000,18), PTF(0b011,28),
        PTF(0b101,12),
        // Those correspond to the "ambiguous" checksums, laid out like the letter disambiguation table
        PTF(0b101, 8), PTF(0b011,10), PTF(0b011,14), PTF(0b000,19), PTF(0b101, 0), PTF(0b001,13), PTF(0b101, 8), PTF(0b001,11), PTF(0b101,12), PTF(0b011, 4), PTF(0b101,12), PTF(0b011,13), PTF(0b100, 7), PTF(0b101,28),
        PTF(0b011, 0), PTF(0b101,20), PTF(0b000,19), PTF(0b011,18), PTF(0b011,28), PTF(0b101,21), PTF(0b101,14), PTF(0b101,14), PTF(0b011,28), PTF(0b011,28), PTF(0b011, 5), PTF(0b101, 2), PTF(0b011,12), PTF(0b011, 4),
        PTF(0b100, 5),
    };

    uint8_t ptf = pal_triplet_ids_and_flags[index];
    return ptf;
}

static void ptf2pal(uint8_t ptf, const uint16_t * out_pal[3])
{
    uint8_t flags = ptf >> 5;
    uint8_t triplet_id = ptf & 0x1f;

    static const uint8_t pal_offsets[] = {
        16*8,   22*8,    8*8,
        17*8,    4*8,   13*8,
        27*8+6,  0*8,   14*8,
        27*8+6,  4*8,   15*8,
         4*8,    4*8,    7*8,

         4*8,   22*8,   18*8,
         4*8,   22*8,   20*8,
        28*8,   22*8,   24*8,
        19*8,   22*8+6,  9*8,
        16*8,   28*8,   10*8,

         3*8+6,  3*8+6, 11*8,
         4*8,   23*8,   28*8,
        17*8,   22*8,    2*8,
         4*8,    0*8,    2*8,
         4*8,   28*8,    3*8,

        28*8,    3*8,    0*8,
         3*8,   28*8,    4*8,
        21*8,   28*8,    4*8,
         3*8,   28*8,    0*8,
         4*8,    3*8,   27*8,

        25*8,    3*8,   28*8,
         0*8,   28*8,    8*8,
         5*8,    5*8,    5*8,
         3*8,   28*8,   12*8,
         4*8,    3*8,   28*8,

         0*8,    0*8,    1*8,
        28*8,    3*8,    6*8,
        26*8,   26*8,   26*8,
         4*8,   28*8,   29*8,
    };

    // This is not stored in the same format as the Game Boy Color
    // boot ROM. **This table here** is RRRRRGGGGGGBBBBB for LVGL's RGB565
    // whereas Game Boy Color's boot ROM table has 0BBBBBGGGGGRRRRR (the MSB is always 0).
    static const uint16_t palettes[] = {
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x15,0x0C), FIVEBITRGB_TO_RGB565(0x10,0x06,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1C,0x18), FIVEBITRGB_TO_RGB565(0x19,0x13,0x10), FIVEBITRGB_TO_RGB565(0x10,0x0D,0x05), FIVEBITRGB_TO_RGB565(0x0B,0x06,0x01),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x11,0x11,0x1B), FIVEBITRGB_TO_RGB565(0x0A,0x0A,0x11), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x0F,0x1F,0x06), FIVEBITRGB_TO_RGB565(0x00,0x10,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x10,0x10), FIVEBITRGB_TO_RGB565(0x12,0x07,0x07), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),

        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x14,0x14,0x14), FIVEBITRGB_TO_RGB565(0x0A,0x0A,0x0A), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x00), FIVEBITRGB_TO_RGB565(0x0F,0x09,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x0F,0x1F,0x00), FIVEBITRGB_TO_RGB565(0x16,0x0E,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x15,0x15,0x10), FIVEBITRGB_TO_RGB565(0x08,0x0E,0x0F), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x14,0x13,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x00), FIVEBITRGB_TO_RGB565(0x00,0x0C,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),

        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x19), FIVEBITRGB_TO_RGB565(0x0C,0x1D,0x1D), FIVEBITRGB_TO_RGB565(0x13,0x10,0x06), FIVEBITRGB_TO_RGB565(0x0B,0x0B,0x0B),
        FIVEBITRGB_TO_RGB565(0x16,0x16,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x12), FIVEBITRGB_TO_RGB565(0x15,0x0B,0x08), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x14), FIVEBITRGB_TO_RGB565(0x1F,0x12,0x12), FIVEBITRGB_TO_RGB565(0x12,0x12,0x1F), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x13), FIVEBITRGB_TO_RGB565(0x12,0x16,0x1F), FIVEBITRGB_TO_RGB565(0x0C,0x12,0x0E), FIVEBITRGB_TO_RGB565(0x00,0x07,0x07),
        FIVEBITRGB_TO_RGB565(0x0D,0x1F,0x00), FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x0A,0x09), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),

        FIVEBITRGB_TO_RGB565(0x0A,0x1B,0x00), FIVEBITRGB_TO_RGB565(0x1F,0x10,0x00), FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x00), FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x0E,0x00), FIVEBITRGB_TO_RGB565(0x12,0x08,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x18,0x08), FIVEBITRGB_TO_RGB565(0x1F,0x1A,0x00), FIVEBITRGB_TO_RGB565(0x12,0x07,0x00), FIVEBITRGB_TO_RGB565(0x09,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x0A,0x1F,0x00), FIVEBITRGB_TO_RGB565(0x1F,0x08,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x0C,0x0A), FIVEBITRGB_TO_RGB565(0x1A,0x00,0x00), FIVEBITRGB_TO_RGB565(0x0C,0x00,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),

        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x13,0x00), FIVEBITRGB_TO_RGB565(0x1F,0x00,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x00,0x1F,0x00), FIVEBITRGB_TO_RGB565(0x06,0x10,0x00), FIVEBITRGB_TO_RGB565(0x00,0x09,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x0B,0x17,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x00,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x1F),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x0F), FIVEBITRGB_TO_RGB565(0x00,0x10,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x00), FIVEBITRGB_TO_RGB565(0x1F,0x00,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),

        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x00), FIVEBITRGB_TO_RGB565(0x1F,0x00,0x00), FIVEBITRGB_TO_RGB565(0x0C,0x00,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x1F,0x19,0x00), FIVEBITRGB_TO_RGB565(0x13,0x0C,0x00), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x00,0x00,0x00), FIVEBITRGB_TO_RGB565(0x00,0x10,0x10), FIVEBITRGB_TO_RGB565(0x1F,0x1B,0x00), FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x0C,0x14,0x1F), FIVEBITRGB_TO_RGB565(0x00,0x00,0x1F), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
        FIVEBITRGB_TO_RGB565(0x1F,0x1F,0x1F), FIVEBITRGB_TO_RGB565(0x0F,0x1F,0x06), FIVEBITRGB_TO_RGB565(0x00,0x0C,0x18), FIVEBITRGB_TO_RGB565(0x00,0x00,0x00),
    };

    const uint8_t * ofs = &pal_offsets[triplet_id * 3];
    // divide by 2 to undo the *2 C does for the 16-bit array type.
    const uint16_t * obp0 = &palettes[ofs[OBP0] / 2];
    const uint16_t * obp1 = &palettes[ofs[OBP1] / 2];
    const uint16_t * bgp  = &palettes[ofs[BGP ] / 2];

    if(flags & 0b001) {
        out_pal[OBP0] = obp0;
    } else {
        out_pal[OBP0] = bgp;
    }

    if(flags & 0b100) {
        out_pal[OBP1] = obp1;
    } else if(flags & 0b010) {
        out_pal[OBP1] = obp0;
    } else {
        out_pal[OBP1] = bgp;
    }

    out_pal[BGP] = bgp;
}

static void title2pal(const uint8_t title[16], const uint16_t * out_pal[3])
{
    uint8_t ptf = title2ptf(title);
    ptf2pal(ptf, out_pal);
}

static void apply_palette(uint16_t dest_lut[0x23 + 1], const uint16_t * const src[3])
{
    for(uint32_t layer_i = 0; layer_i < 3; layer_i++) {
        for(uint32_t shade_i = 0; shade_i < 4; shade_i++) {
            dest_lut[(layer_i << 4) | shade_i] = src[layer_i][shade_i];
        }
    }
}

static void set_automatic_palette(ctx_t * ctx)
{
    uint8_t title[16];
    for(uint32_t i = 0; i < sizeof(title); i++) {
        title[i] = gb_rom_read(&ctx->gb, 0x0134 + i);
    }
    const uint16_t * palette[3];
    title2pal(title, palette);
    apply_palette(ctx->palette, palette);
    ctx->palette_number = -1;
}

static uint8_t gb_rom_read(struct gb_s * gb, const uint_fast32_t addr)
{
    ctx_t * ctx = gb->direct.priv;

    uint32_t shard_i = addr / SHARD_SIZE;
    uint32_t offset = addr % SHARD_SIZE;

    return ctx->rom_shards[shard_i][offset];
}

static uint8_t gb_cart_ram_read(struct gb_s * gb, const uint_fast32_t addr)
{
    ctx_t * ctx = gb->direct.priv;
    return ctx->cart_ram[addr];
}

static void gb_cart_ram_write(struct gb_s * gb, const uint_fast32_t addr, const uint8_t val)
{
    ctx_t * ctx = gb->direct.priv;
    ctx->cart_ram[addr] = val;
}

static void gb_error(struct gb_s*, const enum gb_error_e, const uint16_t)
{
    assert(0);
}

static void lcd_draw_line(struct gb_s *gb,
    const uint8_t *pixels,
    const uint_fast8_t line)
{
    ctx_t * ctx = gb->direct.priv;
    ctx->screenbuf_dirty = true;
    ctx->dirty_rows[line] = true;
    memcpy(ctx->raw_buf[line], pixels, SCRW);
}

static bool get_menu_is_open(lv_obj_t * base_obj)
{
    lv_obj_t * base_obj_youngest_child = lv_obj_get_child(base_obj, -1);
    assert(base_obj_youngest_child);
    return lv_obj_get_class(base_obj_youngest_child) == &lv_list_class;
}

static void update_uinput_joypad(ctx_t * ctx)
{
    ssize_t rwres;

    if(ctx->keypad_fd < 0) {
        return;
    }

    bool menu_is_open_init = false;
    bool menu_is_open;

    struct keyboard_event_s keypad_event;
    while(sizeof(keypad_event) == (rwres = read(ctx->keypad_fd, &keypad_event, sizeof(keypad_event)))) {
        if(!menu_is_open_init) {
            menu_is_open_init = true;
            menu_is_open = get_menu_is_open(ctx->base_obj);
        }
        if(menu_is_open) {
            continue;
        }

        uint8_t mask;
        switch(keypad_event.code) {
            case LV_KEY_UP:
            case 'w':
            case 'W':
                mask = JOYPAD_UP;
                break;
            case LV_KEY_DOWN:
            case 's':
            case 'S':
                mask = JOYPAD_DOWN;
                break;
            case LV_KEY_LEFT:
            case 'a':
            case 'A':
                mask = JOYPAD_LEFT;
                break;
            case LV_KEY_RIGHT:
            case 'd':
            case 'D':
                mask = JOYPAD_RIGHT;
                break;
            case LV_KEY_ESC:
            case LV_KEY_BACKSPACE:
            case 'b':
            case 'B':
                mask = JOYPAD_B;
                break;
            case LV_KEY_ENTER:
                mask = JOYPAD_A;
                break;
            case 'h':
            case 'H':
                mask = JOYPAD_START;
                break;
            case 'g':
            case 'G':
                mask = JOYPAD_SELECT;
                break;
            default:
                open_menu(ctx);
                menu_is_open = true;
                continue;
        }
        if(keypad_event.type == KEYBOARD_PRESS) {
            ctx->uinput_joypad_mask |= mask;
        } else {
            ctx->uinput_joypad_mask &= ~mask;
        }
    }
    assert(rwres < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

    ctx->gb.direct.joypad = ~(ctx->uinput_joypad_mask | ctx->screen_joypad_mask);
}

static void tim_cb(lv_timer_t * tim)
{
    ctx_t * ctx = lv_timer_get_user_data(tim);

    uint32_t overall_ticks = lv_tick_get() - ctx->first_tick;

    double overall_ticks_d = overall_ticks;
    double frames_should_have_run_d = overall_ticks_d * VERTICAL_SYNC / 1000.0;

    uint32_t frames_should_have_run = frames_should_have_run_d;
    uint32_t frames_to_run = frames_should_have_run > ctx->frame_count
                             ? frames_should_have_run - ctx->frame_count
                             : 0;
    if(frames_to_run > 10) {
        frames_to_run = 10;
        lv_timer_ready(tim);
    }
    ctx->frame_count += frames_to_run;

    ctx->screenbuf_dirty = false;
    for(uint32_t i = 0; i < frames_to_run; i++) {
        update_uinput_joypad(ctx);
        gb_run_frame(&ctx->gb);
#ifdef CONFIG_MCP_APPS_PEANUT_GB_SOUND
        flush_sound(ctx);
        minigb_apu_audio_callback(&ctx->apu, ctx->apu_sample_buf);
        ctx->apu_pos = 0;
        ctx->apu_rem = AUDIO_SAMPLES_TOTAL;
        flush_sound(ctx);
#endif
    }
    if(ctx->screenbuf_dirty) {
        for(uint32_t i = 0; i < SCRH; i++) {
            if(ctx->dirty_rows[i]) {
                ctx->dirty_rows[i] = false;
                for(uint32_t j = 0; j < SCRW; j++) {
                    ctx->canv_buf[i][j] = ctx->palette[ctx->raw_buf[i][j]];
                }
            }
        }
        lv_obj_invalidate(ctx->canv);
    }
}

static void btn_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED){
        return;
    }

    ctx_t * ctx = lv_event_get_user_data(e);
    lv_obj_t * btn = lv_event_get_target_obj(e);
    uint8_t mask = (uintptr_t)lv_obj_get_user_data(btn);
    if(code == LV_EVENT_PRESSED) {
        ctx->screen_joypad_mask |= mask;
    } else {
        ctx->screen_joypad_mask &= ~mask;
    }

    ctx->gb.direct.joypad = ~(ctx->uinput_joypad_mask | ctx->screen_joypad_mask);
}

static lv_obj_t * create_pad(lv_obj_t * base_obj, lv_align_t align,
                             const int32_t * col_dsc, const int32_t * row_dsc)
{
    lv_obj_t * pad = lv_obj_create(base_obj);
    lv_obj_set_align(pad, align);
    lv_obj_set_size(pad, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(pad, LV_COORD_MAX, 0);
    lv_obj_set_style_grid_column_dsc_array(pad, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(pad, row_dsc, 0);
    lv_obj_set_style_pad_gap(pad, 0, 0);
    lv_obj_set_layout(pad, LV_LAYOUT_GRID);
    return pad;
}

static void create_left_btn(ctx_t * ctx, lv_obj_t * left, const void * img_src,
                            int32_t col_pos, int32_t row_pos, uint8_t joypad_mask)
{
    lv_obj_t * btn = lv_image_create(left);
    lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col_pos, 1, LV_GRID_ALIGN_STRETCH, row_pos, 1);
    lv_image_set_src(btn, img_src);
    lv_image_set_inner_align(btn, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_add_style(btn, &ctx->btn_style, 0);

    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(btn, (void *)(uintptr_t) joypad_mask);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_ALL, ctx);
}

static void create_right_btn(ctx_t * ctx, lv_obj_t * right,
                             int32_t col_pos, int32_t row_pos, uint8_t joypad_mask)
{
    lv_obj_t * btn = lv_obj_create(right);
    lv_obj_remove_style_all(btn);
    lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col_pos, 1, LV_GRID_ALIGN_STRETCH, row_pos, 1);
    lv_obj_set_style_radius(btn, LV_COORD_MAX, 0);
    lv_obj_add_style(btn, &ctx->btn_style, 0);

    lv_obj_set_user_data(btn, (void *)(uintptr_t) joypad_mask);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_ALL, ctx);
}

static void create_mid_btn(ctx_t * ctx, lv_obj_t * mid, uint8_t joypad_mask)
{
    lv_obj_t * btn = lv_obj_create(mid);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 30, 15);
    lv_obj_set_style_radius(btn, LV_COORD_MAX, 0);
    lv_obj_add_style(btn, &ctx->btn_style, 0);

    lv_obj_set_user_data(btn, (void *)(uintptr_t) joypad_mask);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_ALL, ctx);
}

static void close_menu_cb(lv_event_t * e)
{
    lv_obj_delete(lv_obj_get_parent(lv_event_get_target_obj(e)));
}

static void save_cb(lv_event_t * e)
{
    int res;
    ssize_t rwres;

    ctx_t * ctx = lv_event_get_user_data(e);

    int fd = open(ctx->save_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0);

    uint32_t cart_ram_size = gb_get_save_size(&ctx->gb);
    rwres = write(fd, ctx->cart_ram, cart_ram_size);
    assert(rwres == cart_ram_size);

    res = close(fd);
    assert(res == 0);
}

static void resize_cb(lv_event_t * e)
{
    ctx_t * ctx = lv_event_get_user_data(e);

    lv_obj_t * canv = lv_obj_get_child(ctx->base_obj, 0);

    ++ctx->size_val;
    while(1) {
        switch(ctx->size_val) {
            case 0:
                lv_image_set_inner_align(canv, LV_IMAGE_ALIGN_TOP_MID);
                lv_image_set_scale(canv, LV_SCALE_NONE);
                lv_image_set_offset_y(canv, 0);
                break;
            case 1:
                lv_image_set_inner_align(canv, LV_IMAGE_ALIGN_STRETCH);
                break;
            case 2:
                lv_image_set_inner_align(canv, LV_IMAGE_ALIGN_CONTAIN);
                lv_image_set_offset_y(canv, -((lv_obj_get_height(canv) - lv_image_get_transformed_height(canv)) / 2));
                break;
            default:
                ctx->size_val = 0;
                continue;
        }

        break;
    }
}

static void palette_cb(lv_event_t * e)
{
    ctx_t * ctx = lv_event_get_user_data(e);

    static const uint8_t pal_triplet_ids_and_flags_manual[] = {
        PTF(0b000,18),
        PTF(0b101,16),
        PTF(0b011,25),
        PTF(0b101,24),
        PTF(0b101,13),
        PTF(0b000,22),
        PTF(0b000,23),
        PTF(0b000, 7),
        PTF(0b101,26),
        PTF(0b000, 5),
        PTF(0b011,28),
        PTF(0b000,19),
    };

    int8_t palette_number = ++ctx->palette_number;

    if(palette_number < sizeof(pal_triplet_ids_and_flags_manual)) {
        uint8_t ptf = pal_triplet_ids_and_flags_manual[palette_number];
        const uint16_t * palette[3];
        ptf2pal(ptf, palette);
        apply_palette(ctx->palette, palette);
    }
    else {
        set_automatic_palette(ctx);
    }
}

static void quit_cb(lv_event_t * e)
{
    ctx_t * ctx = lv_event_get_user_data(e);
    lv_obj_delete(ctx->base_obj);
}

static void open_menu(ctx_t * ctx)
{
    lv_obj_t * base_obj = ctx->base_obj;
    if(get_menu_is_open(base_obj)) {
        return;
    }

    lv_obj_t * menu = lv_list_create(base_obj);
    lv_gridnav_add(menu, LV_GRIDNAV_CTRL_NONE);
    lv_group_add_obj(lv_group_get_default(), menu);
    lv_obj_center(menu);

    lv_obj_t * btn;

    btn = lv_list_add_button(menu, LV_SYMBOL_PLAY, "Resume");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, close_menu_cb, LV_EVENT_CLICKED, NULL);

    btn = lv_list_add_button(menu, LV_SYMBOL_SAVE, "Save");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, save_cb, LV_EVENT_CLICKED, ctx);

    btn = lv_list_add_button(menu, LV_SYMBOL_IMAGE, "Resize");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, resize_cb, LV_EVENT_CLICKED, ctx);

    btn = lv_list_add_button(menu, LV_SYMBOL_EYE_OPEN, "Palette");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, palette_cb, LV_EVENT_CLICKED, ctx);

    btn = lv_list_add_button(menu, LV_SYMBOL_POWER, "Quit");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, quit_cb, LV_EVENT_CLICKED, ctx);
}

static void menu_btn_clicked_cb(lv_event_t * e)
{
    ctx_t * ctx = lv_event_get_user_data(e);
    open_menu(ctx);
}

static void create_sceen_joypad(lv_obj_t * base_obj)
{
    ctx_t * ctx = lv_obj_get_user_data(base_obj);

    static const int32_t left_col_dsc[] = {30, 30, 30, LV_GRID_TEMPLATE_LAST};
    static const int32_t left_row_dsc[] = {30, 30, 30, LV_GRID_TEMPLATE_LAST};
    lv_obj_t * left = create_pad(base_obj, LV_ALIGN_BOTTOM_LEFT, left_col_dsc, left_row_dsc);
    create_left_btn(ctx, left, LV_SYMBOL_UP, 1, 0, JOYPAD_UP);
    create_left_btn(ctx, left, LV_SYMBOL_DOWN, 1, 2, JOYPAD_DOWN);
    create_left_btn(ctx, left, LV_SYMBOL_LEFT, 0, 1, JOYPAD_LEFT);
    create_left_btn(ctx, left, LV_SYMBOL_RIGHT, 2, 1, JOYPAD_RIGHT);

    static const int32_t right_col_dsc[] = {45, 45, LV_GRID_TEMPLATE_LAST};
    static const int32_t right_row_dsc[] = {45, 45, LV_GRID_TEMPLATE_LAST};
    lv_obj_t * right = create_pad(base_obj, LV_ALIGN_BOTTOM_RIGHT, right_col_dsc, right_row_dsc);
    create_right_btn(ctx, right, 1, 0, JOYPAD_A);
    create_right_btn(ctx, right, 0, 1, JOYPAD_B);

    lv_obj_t * mid = lv_obj_create(base_obj);
    lv_obj_align(mid, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(mid, LV_COORD_MAX, 0);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(mid, 5, 0);

    create_mid_btn(ctx, mid, JOYPAD_SELECT);
    create_mid_btn(ctx, mid, JOYPAD_START);

    lv_obj_t * menu_btn = lv_image_create(base_obj);
    lv_obj_align(menu_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_image_set_src(menu_btn, LV_SYMBOL_LIST);
    lv_obj_add_flag(menu_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(menu_btn, menu_btn_clicked_cb, LV_EVENT_CLICKED, ctx);
}

static void game_file_chosen_cb(lv_event_t * e)
{
    int res;
    ssize_t rwres;
    struct stat statbuf;

    lv_obj_t * fe = lv_event_get_target_obj(e);
    lv_obj_t * base_obj = lv_obj_get_parent(fe);
    ctx_t * ctx = lv_obj_get_user_data(base_obj);

    char * rom_path;
    res = asprintf(&rom_path, "%s%s",
                   lv_file_explorer_get_current_path(fe),
                   lv_file_explorer_get_selected_file_name(fe));
    assert(res >= 0);
    free(ctx->save_path);
    res = asprintf(&ctx->save_path, "%s.sav", rom_path);
    assert(res >= 0);
    int fd = open(rom_path, O_RDONLY);
    free(rom_path);

    if(fd < 0) {
        fprintf(stderr, "could not open ROM\n");
        return;
    }

    res = fstat(fd, &statbuf);
    assert(res == 0);
    uint32_t rom_size = statbuf.st_size;
    if(rom_size > MAX_ROM_SIZE) {
        fprintf(stderr, "ROM too big\n");
        res = close(fd);
        assert(res == 0);
        return;
    }

    uint32_t read_size_remaining = rom_size;
    for(uint32_t i = 0; read_size_remaining; i++) {
        uint8_t * shard = malloc(SHARD_SIZE);
        assert(shard);
        ctx->rom_shards[i] = shard;
        uint32_t this_read_size = read_size_remaining < SHARD_SIZE ? read_size_remaining : SHARD_SIZE;
        rwres = read(fd, shard, this_read_size);
        assert(rwres == this_read_size);
        read_size_remaining -= this_read_size;
    }

    res = close(fd);
    assert(res == 0);

    lv_style_init(&ctx->btn_style);
    lv_style_set_border_opa(&ctx->btn_style, LV_OPA_COVER);
    lv_style_set_border_width(&ctx->btn_style, 2);
    lv_style_set_border_color(&ctx->btn_style, lv_color_black());
    ctx->style_is_init = true;

    lv_obj_clean(base_obj);
    lv_obj_set_style_layout(base_obj, LV_LAYOUT_NONE, 0);

#ifdef CONFIG_MCP_APPS_PEANUT_GB_SOUND
    setup_sound(ctx);
#endif

    enum gb_init_error_e err = gb_init(&ctx->gb,
                                       gb_rom_read,
                                       gb_cart_ram_read,
                                       gb_cart_ram_write,
                                       gb_error,
                                       ctx);
    assert(err == 0);
    gb_init_lcd(&ctx->gb, lcd_draw_line);

    // ctx->gb.direct.interlace = true;
    ctx->gb.direct.frame_skip = true;

    uint32_t cart_ram_size = gb_get_save_size(&ctx->gb);
    if(cart_ram_size) {
        ctx->cart_ram = malloc(cart_ram_size);
        assert(ctx->cart_ram);

        fd = open(ctx->save_path, O_RDONLY);
        if(fd < 0) {
            assert(errno == ENOENT);
            memset(ctx->cart_ram, 0xff, cart_ram_size);
        }
        else {
            res = fstat(fd, &statbuf);
            assert(res == 0);
            assert(statbuf.st_size == cart_ram_size);
            rwres = read(fd, ctx->cart_ram, cart_ram_size);
            assert(rwres == cart_ram_size);
        }
    }

    lv_obj_t * canv = lv_canvas_create(base_obj);
    lv_image_set_inner_align(canv, LV_IMAGE_ALIGN_TOP_MID);
    lv_obj_set_size(canv, LV_PCT(100), LV_PCT(100));
    lv_canvas_set_buffer(canv, ctx->canv_buf, SCRW, SCRH, LV_COLOR_FORMAT_RGB565);

    ctx->keypad_fd = open("/dev/ukeyboard", O_RDONLY | O_NONBLOCK);
    assert(ctx->keypad_fd >= 0 || errno == ENOENT);

    lv_indev_t * indev = NULL;
    while((indev = lv_indev_get_next(indev))) {
        if(lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            create_sceen_joypad(base_obj);
            break;
        }
    }

    ctx->first_tick = lv_tick_get();
    ctx->frame_count = 1;
    ctx->canv = canv;

    set_automatic_palette(ctx);

    update_uinput_joypad(ctx);
    gb_run_frame(&ctx->gb);
    ctx->timer = lv_timer_create(tim_cb, GB_TIMER_PERIOD, ctx);
}

static void base_obj_delete_cb(lv_event_t * e)
{
    lv_obj_t * base_obj = lv_event_get_target_obj(e);;
    ctx_t * ctx = lv_obj_get_user_data(base_obj);
#ifdef CONFIG_MCP_APPS_PEANUT_GB_SOUND
    teardown_sound(ctx);
#endif
    for(uint32_t i = 0; i < SHARD_COUNT; i++) {
        if(ctx->rom_shards[i] == NULL) break;
        free(ctx->rom_shards[i]);
    }
    free(ctx->cart_ram);
    free(ctx->save_path);
    if(ctx->timer) lv_timer_delete(ctx->timer);
    if(ctx->keypad_fd >= 0) assert(0 == close(ctx->keypad_fd));
    if(ctx->style_is_init) lv_style_reset(&ctx->btn_style);
    free(ctx);
}

void peanut_gb_app_run(lv_obj_t * base_obj)
{
    ctx_t * ctx = calloc(1, sizeof(*ctx));
    assert(ctx);
    ctx->base_obj = base_obj;
    ctx->keypad_fd = -1;
    lv_obj_set_user_data(base_obj, ctx);
    lv_obj_add_event_cb(base_obj, base_obj_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_set_flex_flow(base_obj, LV_FLEX_COLUMN);
    lv_obj_set_flex_align(base_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * label = lv_label_create(base_obj);
    lv_label_set_text_static(label, "pick a game");

    lv_obj_t * fe = lv_file_explorer_create(base_obj);
    lv_obj_set_flex_grow(fe, 1);
    lv_file_explorer_open_dir(fe, "");
    lv_obj_add_event_cb(fe, game_file_chosen_cb, LV_EVENT_VALUE_CHANGED, NULL);
}
