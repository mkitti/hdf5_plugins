/*
 * ZSTD HDF5 filter
 *
 * Author: Mark Rivers <rivers@cars.uchicago.edu>
 * Created: 2019
 *
 *
 */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "H5PLextern.h"

#include "zstd.h"

static size_t H5Z_filter_zstd(unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[],
                              size_t nbytes, size_t *buf_size, void **buf);

#define H5Z_FILTER_ZSTD 32015

#define PUSH_ERR(func, minor, str)                                                                           \
    H5Epush(H5E_DEFAULT, __FILE__, func, __LINE__, H5E_ERR_CLS, H5E_PLINE, minor, str)
#define PUSH_ERR2(func, minor, str, arg)                                                                     \
    H5Epush(H5E_DEFAULT, __FILE__, func, __LINE__, H5E_ERR_CLS, H5E_PLINE, minor, str, arg)

/*
 * Decompress a zstd frame whose decompressed size is not known ahead of
 * time (e.g. produced by another implementation via zstd's streaming
 * compression API). Grows *outbuf_ptr as needed. On success returns 0 with
 * *outbuf_ptr set to a malloc'd buffer of exactly *outSize_ptr bytes, which
 * becomes the caller's responsibility to free. On failure returns -1,
 * frees any internally allocated buffer, and pushes an HDF5 error.
 */
static int
H5Z_zstd_decompress_stream(const void *src, size_t srcSize, void **outbuf_ptr, size_t *outSize_ptr)
{
    ZSTD_DStream *dstream       = NULL;
    void         *outbuf        = NULL;
    size_t        outCapacity   = ZSTD_DStreamOutSize();
    size_t        outUsed       = 0;
    ZSTD_inBuffer input         = {src, srcSize, 0};
    int           frameComplete = 0;
    int           ret           = -1;

    if (NULL == (dstream = ZSTD_createDStream())) {
        PUSH_ERR("H5Z_zstd_decompress_stream", H5E_CALLBACK, "Can't create zstd decompression stream");
        goto done;
    }

    size_t const initResult = ZSTD_initDStream(dstream);
    if (ZSTD_isError(initResult)) {
        PUSH_ERR2("H5Z_zstd_decompress_stream", H5E_CALLBACK,
                  "Can't initialize zstd decompression stream: %s", ZSTD_getErrorName(initResult));
        goto done;
    }

    if (NULL == (outbuf = malloc(outCapacity))) {
        PUSH_ERR("H5Z_zstd_decompress_stream", H5E_CALLBACK, "Can't allocate zstd decompression buffer");
        goto done;
    }

    while (!frameComplete) {
        ZSTD_outBuffer output = {outbuf, outCapacity, outUsed};
        size_t const   result = ZSTD_decompressStream(dstream, &output, &input);

        if (ZSTD_isError(result)) {
            PUSH_ERR2("H5Z_zstd_decompress_stream", H5E_CALLBACK, "zstd streaming decompression failed: %s",
                      ZSTD_getErrorName(result));
            goto done;
        }

        outUsed = output.pos;

        if (result == 0) {
            /* Frame is fully decoded and flushed. */
            frameComplete = 1;
        }
        else if (output.pos == output.size) {
            /* Output buffer is full but the frame isn't finished; grow it. */
            void  *newbuf;
            size_t newCapacity = outCapacity * 2;

            if (newCapacity <= outCapacity) {
                PUSH_ERR("H5Z_zstd_decompress_stream", H5E_CALLBACK,
                         "zstd decompression buffer size overflow");
                goto done;
            }
            if (NULL == (newbuf = realloc(outbuf, newCapacity))) {
                PUSH_ERR("H5Z_zstd_decompress_stream", H5E_CALLBACK, "Can't grow zstd decompression buffer");
                goto done;
            }
            outbuf      = newbuf;
            outCapacity = newCapacity;
        }
        else if (input.pos == input.size) {
            /* No more input, but zstd says the frame isn't done. */
            PUSH_ERR("H5Z_zstd_decompress_stream", H5E_CALLBACK,
                     "zstd stream ended before frame was complete (truncated data?)");
            goto done;
        }
        /* else: output buffer has room and input remains; keep decoding. */
    }

    *outbuf_ptr  = outbuf;
    *outSize_ptr = outUsed;
    outbuf       = NULL; /* ownership transferred to caller */
    ret          = 0;

done:
    if (dstream)
        ZSTD_freeDStream(dstream);
    if (outbuf)
        free(outbuf);
    return ret;
}

/*
 * Decompress a zstd-compressed chunk. On success returns 0 with
 * *outbuf_ptr set to a malloc'd buffer of *outSize_ptr bytes, which
 * becomes the caller's responsibility to free. On failure returns -1 and
 * pushes an HDF5 error; *outbuf_ptr is left untouched.
 */
static int
H5Z_zstd_decompress(const void *src, size_t srcSize, void **outbuf_ptr, size_t *outSize_ptr)
{
    unsigned long long contentSize = ZSTD_getFrameContentSize(src, srcSize);
    void              *outbuf;
    size_t             decompSize;

    if (contentSize == ZSTD_CONTENTSIZE_ERROR) {
        PUSH_ERR("H5Z_zstd_decompress", H5E_CALLBACK, "Input is not a valid zstd frame");
        return -1;
    }

    if (contentSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        /*
         * The frame doesn't carry its decompressed size, e.g. because it
         * was produced by another implementation using zstd's streaming
         * compression API. Fall back to streaming decompression, which
         * doesn't need the size up front.
         */
        return H5Z_zstd_decompress_stream(src, srcSize, outbuf_ptr, outSize_ptr);
    }

    if (contentSize == 0) {
        PUSH_ERR("H5Z_zstd_decompress", H5E_CALLBACK, "zstd frame has zero decompressed size");
        return -1;
    }

    if (NULL == (outbuf = malloc((size_t)contentSize))) {
        PUSH_ERR("H5Z_zstd_decompress", H5E_CALLBACK, "Can't allocate zstd decompression buffer");
        return -1;
    }

    decompSize = ZSTD_decompress(outbuf, (size_t)contentSize, src, srcSize);
    if (ZSTD_isError(decompSize)) {
        PUSH_ERR2("H5Z_zstd_decompress", H5E_CALLBACK, "zstd decompression failed: %s",
                  ZSTD_getErrorName(decompSize));
        free(outbuf);
        return -1;
    }

    *outbuf_ptr  = outbuf;
    *outSize_ptr = decompSize;
    return 0;
}

const H5Z_class2_t H5Z_ZSTD[1] = {{
    H5Z_CLASS_T_VERS,              /* H5Z_class_t version */
    (H5Z_filter_t)H5Z_FILTER_ZSTD, /* Filter id number             */
#ifdef FILTER_DECODE_ONLY
    0, /* encoder_present flag (false is not available) */
#else
    1, /* encoder_present flag (set to true) */
#endif
    1, /* decoder_present flag (set to true) */
    "HDF5 zstd filter; see "
    "https://github.com/HDFGroup/hdf5_plugins/blob/master/docs/RegisteredFilterPlugins.md",
    /* Filter name for debugging    */
    NULL,                        /* The "can apply" callback     */
    NULL,                        /* The "set local" callback     */
    (H5Z_func_t)H5Z_filter_zstd, /* The actual filter function   */
}};

H5PL_type_t
H5PLget_plugin_type(void)
{
    return H5PL_TYPE_FILTER;
}
const void *
H5PLget_plugin_info(void)
{
    return H5Z_ZSTD;
}

static size_t
H5Z_filter_zstd(unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], size_t nbytes,
                size_t *buf_size, void **buf)
{
    size_t buf_size_out = 0;
    size_t origSize     = nbytes; /* Number of bytes for output (compressed) buffer */
    void  *outbuf       = NULL;
    void  *inbuf        = NULL; /* Pointer to input buffer */
    inbuf               = *buf;

    if (flags & H5Z_FLAG_REVERSE) {
        /* We're decompressing */
        size_t decompSize;

        if (0 != H5Z_zstd_decompress(inbuf, origSize, &outbuf, &decompSize))
            goto error;

#ifdef ZSTD_DEBUG
        fprintf(stderr, "   decompressing nbytes: %ld\n", decompSize);
#endif

        buf_size_out = decompSize;
    }
    else {
        /* We're compressing */
        /*
         * cd_values[0] = aggression
         *
         * As of Zstandard v1.5.7
         * ZSTD_minCLevel() == -1<<17 == -131072
         * ZSTD_maxCLevel() == 22
         *
         * Negative compression levels are faster at the cost of compression
         * aggression >= 20 require more memory
         */
        int aggression;
        if (cd_nelmts > 0)
            aggression = (int)cd_values[0];
        else
            aggression = ZSTD_CLEVEL_DEFAULT;
        if (aggression < ZSTD_minCLevel())
            aggression = ZSTD_minCLevel();
        else if (aggression > ZSTD_maxCLevel())
            aggression = ZSTD_maxCLevel();

        size_t compSize = ZSTD_compressBound(origSize);
        if (NULL == (outbuf = malloc(compSize))) {
            PUSH_ERR("H5Z_filter_zstd", H5E_CALLBACK, "Can't allocate zstd compression buffer");
            goto error;
        }

        compSize = ZSTD_compress(outbuf, compSize, inbuf, origSize, aggression);
        if (ZSTD_isError(compSize)) {
            PUSH_ERR2("H5Z_filter_zstd", H5E_CALLBACK, "zstd compression failed: %s",
                      ZSTD_getErrorName(compSize));
            goto error;
        }

#ifdef ZSTD_DEBUG
        fprintf(stderr, "    compressing nbytes: %ld\n", compSize);
#endif

        buf_size_out = compSize;
    }
    free(*buf);
    *buf      = outbuf;
    *buf_size = buf_size_out;
    return buf_size_out;

error:
    if (outbuf)
        free(outbuf);
    return 0;
}
