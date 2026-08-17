/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of the HDF5 ZSTD filter plugin source.  The full       *
 * copyright notice, including terms governing use, modification, and        *
 * terms governing use, modification, and redistribution, is contained in    *
 * the file COPYING, which can be found at the root of the ZSTD source code *
 * distribution tree.  If you do not have access to this file, you may       *
 * request a copy from help@hdfgroup.org.                                    *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/************************************************************

  This test produces a zstd frame with zstd's streaming compression API
  (ZSTD_compressStream2 with the content-size flag disabled), so the
  frame has no embedded decompressed size -- the same situation as data
  written by a different HDF5 zstd plugin implementation that uses
  zstd's streaming API. That frame is injected directly into an HDF5
  chunk via H5Dwrite_chunk, bypassing this plugin's own encoder (which
  always embeds a decompressed size).

  Reading the dataset back exercises this plugin's streaming
  decompression fallback, used whenever ZSTD_getFrameContentSize()
  reports ZSTD_CONTENTSIZE_UNKNOWN.

 ************************************************************/

#include "hdf5.h"
#include "zstd.h"
#include <stdio.h>
#include <stdlib.h>

#define FILENAME        "h5ex_d_zstd_streaming.h5"
#define DATASET         "DS1"
#define DIM0            307200 /* larger than ZSTD_DStreamOutSize(), to exercise buffer growth */
#define CHUNK0          DIM0
#define H5Z_FILTER_ZSTD 32015

/*
 * Compress src into a zstd frame with no embedded content size, as zstd's
 * streaming compression API produces. Returns a malloc'd buffer via
 * *outbuf (caller frees) and its size via *outSize on success (0), or -1
 * on failure.
 */
static int
compress_streaming(const void *src, size_t srcSize, void **outbuf, size_t *outSize)
{
    ZSTD_CCtx     *cctx        = NULL;
    void          *dst         = NULL;
    size_t         dstCapacity = ZSTD_compressBound(srcSize);
    ZSTD_inBuffer  input       = {src, srcSize, 0};
    ZSTD_outBuffer output;
    size_t         remaining;
    int            ret = -1;

    if (NULL == (dst = malloc(dstCapacity)))
        goto done;

    if (NULL == (cctx = ZSTD_createCCtx()))
        goto done;

    /* Disable the content-size flag so the frame requires a streaming
     * decompressor, i.e. ZSTD_getFrameContentSize() on it returns
     * ZSTD_CONTENTSIZE_UNKNOWN. */
    if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0)))
        goto done;

    output.dst  = dst;
    output.size = dstCapacity;
    output.pos  = 0;

    do {
        remaining = ZSTD_compressStream2(cctx, &output, &input, ZSTD_e_end);
        if (ZSTD_isError(remaining))
            goto done;
    } while (remaining != 0);

    if (ZSTD_getFrameContentSize(dst, output.pos) != ZSTD_CONTENTSIZE_UNKNOWN) {
        fprintf(stderr, "Test setup error: frame unexpectedly has a known content size\n");
        goto done;
    }

    *outbuf  = dst;
    *outSize = output.pos;
    dst      = NULL; /* ownership transferred */
    ret      = 0;

done:
    if (cctx)
        ZSTD_freeCCtx(cctx);
    if (dst)
        free(dst);
    return ret;
}

int
main(void)
{
    hid_t          file_id  = H5I_INVALID_HID;
    hid_t          space_id = H5I_INVALID_HID;
    hid_t          dset_id  = H5I_INVALID_HID;
    hid_t          dcpl_id  = H5I_INVALID_HID;
    herr_t         status;
    htri_t         avail;
    hsize_t        dims[1] = {DIM0}, chunk[1] = {CHUNK0}, offset[1] = {0};
    unsigned char *wdata = NULL, *rdata = NULL;
    void          *cbuf  = NULL;
    size_t         csize = 0;
    hsize_t        i;
    int            num_diff  = 0;
    int            ret_value = 1;

    wdata = (unsigned char *)malloc(DIM0);
    rdata = (unsigned char *)malloc(DIM0);
    if (!wdata || !rdata) {
        fprintf(stderr, "failed to allocate data buffers.\n");
        goto done;
    }

    for (i = 0; i < DIM0; i++)
        wdata[i] = (unsigned char)i;

    printf("....Compressing data with zstd's streaming API (no content size) ................\n");
    if (0 != compress_streaming(wdata, DIM0, &cbuf, &csize)) {
        fprintf(stderr, "failed to produce a streaming-compressed zstd frame.\n");
        goto done;
    }

    file_id = H5Fcreate(FILENAME, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0)
        goto done;

    space_id = H5Screate_simple(1, dims, NULL);
    if (space_id < 0)
        goto done;

    dcpl_id = H5Pcreate(H5P_DATASET_CREATE);
    if (dcpl_id < 0)
        goto done;

    status = H5Pset_filter(dcpl_id, H5Z_FILTER_ZSTD, H5Z_FLAG_MANDATORY, 0, NULL);
    if (status < 0)
        goto done;

    avail = H5Zfilter_avail(H5Z_FILTER_ZSTD);
    if (!avail) {
        printf("H5Zfilter_avail - not found.\n");
        goto done;
    }

    status = H5Pset_chunk(dcpl_id, 1, chunk);
    if (status < 0)
        goto done;

    printf("....Create dataset ................\n");
    dset_id = H5Dcreate(file_id, DATASET, H5T_NATIVE_UINT8, space_id, H5P_DEFAULT, dcpl_id, H5P_DEFAULT);
    if (dset_id < 0) {
        printf("failed to create dataset.\n");
        goto done;
    }

    /*
     * Inject the streaming-compressed chunk directly, bypassing this
     * plugin's own encoder entirely.
     */
    printf("....Writing pre-compressed streaming chunk directly ................\n");
    status = H5Dwrite_chunk(dset_id, H5P_DEFAULT, 0, offset, csize, cbuf);
    if (status < 0) {
        printf("failed to write chunk.\n");
        goto done;
    }

    H5Dclose(dset_id);
    dset_id = -1;
    H5Pclose(dcpl_id);
    dcpl_id = -1;
    H5Sclose(space_id);
    space_id = -1;
    H5Fclose(file_id);
    file_id = -1;
    status  = H5close();
    if (status < 0) {
        printf("\nFAILED to close library\n");
        goto done;
    }

    printf("....Close the file and reopen for reading ........\n");

    file_id = H5Fopen(FILENAME, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0)
        goto done;

    dset_id = H5Dopen(file_id, DATASET, H5P_DEFAULT);
    if (dset_id < 0)
        goto done;

    /*
     * Reading triggers the zstd plugin's decompression path. Since the
     * chunk we wrote has no embedded content size, this exercises the
     * streaming-decompression fallback added for ZSTD_CONTENTSIZE_UNKNOWN.
     */
    printf("....Reading data decompressed via the zstd streaming fallback ................\n");
    status = H5Dread(dset_id, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, rdata);
    if (status < 0) {
        printf("failed to read data.\n");
        goto done;
    }

    for (i = 0; i < DIM0; i++) {
        if (rdata[i] != wdata[i])
            num_diff++;
    }
    printf("ZSTD streaming test: number of differing array elements=%d\n", num_diff);

    if (num_diff != 0) {
        printf("FAILED: decompressed data does not match original data.\n");
        goto done;
    }

    printf("PASSED\n");
    ret_value = 0;

done:
    free(wdata);
    free(rdata);
    free(cbuf);
    if (dcpl_id >= 0)
        H5Pclose(dcpl_id);
    if (dset_id >= 0)
        H5Dclose(dset_id);
    if (space_id >= 0)
        H5Sclose(space_id);
    if (file_id >= 0)
        H5Fclose(file_id);

    return ret_value;
}
