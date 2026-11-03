#ifndef PHF_H
#define PHF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(PHF_BUILD_DLL)
  #define PHF_API __declspec(dllexport)
#elif defined(_WIN32)
  #define PHF_API
#else
  #define PHF_API __attribute__((visibility("default")))
#endif

#define PHF_VERSION 1
#define PHF_MAGIC 0x31464850 /* "PHF1" little-endian */
#define PHF_HEADER_SIZE 20
#define PHF_CRC_SIZE 4

/* Header flag bits (byte 0x06). */
#define PHF_FLAG_ALPHA  0x01 /* RGBA (channels == 4) */
#define PHF_FLAG_MED    0x02 /* spatial MED predictor */
#define PHF_FLAG_DECORR 0x04 /* R-G, G, B-G color decorrelation */
#define PHF_FLAG_CRC32  0x08 /* CRC-32 of the payload follows it (4 bytes, LE) */

/* Encoder limits (the inner NHF3 stream stores 16-bit dimensions). */
#define PHF_MAX_DIM    65535u
#define PHF_MAX_PIXELS (1u << 26)

/* Error codes returned by the int-returning functions. */
#define PHF_OK             0
#define PHF_ERR_ARG       -1 /* NULL pointer / invalid argument */
#define PHF_ERR_MAGIC     -2 /* not a PHF file */
#define PHF_ERR_VERSION   -3 /* unsupported container version */
#define PHF_ERR_SIZE      -4 /* width/height do not match the file */
#define PHF_ERR_TRUNCATED -5 /* buffer shorter than the header says */
#define PHF_ERR_STRIDE    -6 /* stride < channels */
#define PHF_ERR_CORRUPT   -7 /* malformed or inconsistent payload */
#define PHF_ERR_CHECKSUM  -8 /* payload CRC-32 mismatch */
#define PHF_ERR_MEM       -9 /* allocation failure */
#define PHF_ERR_IO       -10 /* file open/write failure */

/* Coding mode. MED predicts each colour plane from its neighbours: best for photos,
 * camera and video. PALETTE codes whole pixels as indices into the image's colours:
 * best (and faster) for graphics, screenshots and UI with few colours. AUTO picks
 * per image; see phf_effort_t. */
typedef enum {
    PHF_MODE_AUTO = 0,
    PHF_MODE_MED = 1,
    PHF_MODE_PALETTE = 2
} phf_mode_t;

/* Encoder effort. BEST is for files: AUTO encodes both modes when the image has at
 * most 4096 colours and keeps the smaller one, and each plane tries both of its
 * variants. FAST is for streaming: one encode, no trials; AUTO uses PALETTE only
 * for images with at most 256 colours. Decoding speed does not depend on it. */
typedef enum {
    PHF_EFFORT_BEST = 0,
    PHF_EFFORT_FAST = 1
} phf_effort_t;

/* A zero-initialised struct (or a NULL pointer) means { AUTO, BEST }. */
typedef struct {
    phf_mode_t mode;
    phf_effort_t effort;
} phf_encode_options_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t  channels;  /* 3 = RGB, 4 = RGBA */
    uint8_t  flags;     /* PHF_FLAG_* */
    size_t   payload_size;
} phf_info_t;

/**
 * phf_probe_memory:
 *   Reads the 20-byte PHF file header and populates out_info.
 *   Only needs the first 20 bytes; does not validate the payload.
 *   Returns PHF_OK on success, or a negative PHF_ERR_* code.
 */
PHF_API int phf_probe_memory(const uint8_t *data, size_t size, phf_info_t *out_info);

/**
 * phf_encode_memory:
 *   Encodes an RGB or RGBA pixel buffer into the full PHF file container (.phf).
 *   The returned pointer includes the 20-byte header, the N-Huffman compressed
 *   payload and a 4-byte CRC-32 trailer. Must be freed with phf_free().
 *   options may be NULL for { PHF_MODE_AUTO, PHF_EFFORT_BEST }; streaming callers
 *   usually want a fixed mode with PHF_EFFORT_FAST.
 *   Width and height must each be <= PHF_MAX_DIM and width*height <= PHF_MAX_PIXELS;
 *   returns NULL otherwise, on an invalid option, or on allocation failure.
 */
PHF_API uint8_t* phf_encode_memory(
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const phf_encode_options_t *options,
    size_t *out_size
);

/**
 * phf_decode_memory:
 *   Decodes a complete in-memory PHF file buffer into destination pixel buffer.
 *   stride: Byte step between consecutive pixels in dst_pixels.
 *           Common values: 3 (tightly packed RGB) or 4 (RGBA / Canvas / Direct3D / Vulkan).
 *           If 0 is passed, stride defaults to the PHF file's native channel count.
 *           If stride >= 4 and source is RGB (channels == 3), alpha is set to 255 (opaque).
 *   dst_pixels must hold width * height * stride bytes.
 *   If the file carries a CRC-32 (PHF_FLAG_CRC32) it is verified before decoding.
 *   Returns PHF_OK on success, or a negative PHF_ERR_* code.
 */
PHF_API int phf_decode_memory(
    const uint8_t *phf_data,
    size_t phf_size,
    uint8_t *dst_pixels,
    uint32_t width,
    uint32_t height,
    uint8_t stride
);

/**
 * phf_write_file:
 *   Encodes and writes an RGB/RGBA image directly to a .phf file on disk.
 *   options as in phf_encode_memory (NULL = AUTO, BEST).
 *   Returns PHF_OK on success, or a negative PHF_ERR_* code.
 */
PHF_API int phf_write_file(
    const char *filepath,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    uint8_t channels,
    const phf_encode_options_t *options
);

/**
 * phf_read_file:
 *   Reads and decodes a .phf file from disk, allocating and returning the pixel buffer.
 *   Fills out_info with dimensions. Must be freed with phf_free().
 */
PHF_API uint8_t* phf_read_file(const char *filepath, phf_info_t *out_info);

/**
 * phf_free:
 *   Frees memory allocated by libphf.
 */
PHF_API void phf_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* PHF_H */
