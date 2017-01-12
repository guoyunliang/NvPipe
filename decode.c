/* Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/* This file implements the decode side of NvPipe.  It expects a valid h264
 * stream, particularly one generated by 'encode.c'.
 * Most of the involved parts of the logic come from sizing: we have multiple
 * sizes of relevance.  1) The size we expected images to be when creating the
 * decoder; 2) the size the user wanted when creating the decoder; 3) the size
 * of the image coming from the stream, and 4) the size that the user wants
 * /now/.  Because windows might be resized, (1) is not always == (3) and
 * (2) is not always == (4).
 * Worse, typically one has a frame or more of latency.  So a resize operation
 * will change (4) in frame N and then (3) in N+x. */
#define _POSIX_C_SOURCE 201212L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <cuda_runtime_api.h>
#include <nvcuvid.h>
#include <nvToolsExt.h>
#include "config.nvp.h"
#include "debug.h"
#include "internal-api.h"
#include "nvpipe.h"
#include "yuv.h"

/* NvDec can actually do 8kx8k for H.264/HEVC, but this library does not yet
 * support that codec anyway. */
const size_t MAX_WIDTH = 4096;
const size_t MAX_HEIGHT = 4096;

DECLARE_CHANNEL(dec);

struct nvp_decoder {
	nvp_impl_t impl;
	CUcontext ctx;
	bool initialized;
	CUvideodecoder decoder;
	CUvideoparser parser;
	/** Most of the involved parts of the logic in this file is sizing.  There are
	 * multiple: 1) The size we expected images to be when creating the decoder;
	 * 2) the size the user wanted when creating the decoder; 3) the size of the
	 * image coming from the stream, and 4) the size that the user wants /now/.
	 * Because windows might be resized, (1) is not always == (3) and (2) is not
	 * always == (4). */
	struct {
		size_t wi; /* what input/source dims Decoder was created with (1) */
		size_t hi;
		size_t wdst; /* what *target* dims Decoder was created with (2) */
		size_t hdst;
		size_t wsrc; /* "source" width/height: what DecodePicture says. (3) */
		size_t hsrc;
		/* 4 is not explicitly stored: this will be the argument to _decode. */
	} d; /* for "dims" */
	CUdeviceptr rgb; /**< temporary buffer to hold converted data. */
	bool empty;
	nv_fut_t* reorg; /**< reorganizes data from nv12 to RGB form. */
};

static int dec_sequence(void* cdc, CUVIDEOFORMAT* fmt);
static int dec_ode(void* cdc, CUVIDPICPARAMS* pic);

/** Initialize or reinitialize the decoder.
 * @param iwidth the input width of images
 * @param iheight the input height of images
 * @param dstwidth user width; width of image the user requested
 * @param dstheight user height; height of image the user requested */
static bool
dec_initialize(struct nvp_decoder* nvp, size_t iwidth, size_t iheight,
               size_t dstwidth, size_t dstheight) {
	assert(iwidth > 0 && iheight > 0);
	assert(dstwidth > 0 && dstheight > 0);
	assert(nvp->decoder == NULL);
	CUVIDDECODECREATEINFO crt = {0};
	crt.CodecType = cudaVideoCodec_H264;
	crt.ulWidth = iwidth;
	crt.ulHeight = iheight;
	nvp->d.wi = iwidth;
	nvp->d.hi = iheight;
	crt.ulNumDecodeSurfaces = 1;
	crt.ChromaFormat = cudaVideoChromaFormat_420;
	crt.OutputFormat = cudaVideoSurfaceFormat_NV12;
	crt.DeinterlaceMode = cudaVideoDeinterlaceMode_Adaptive;
	crt.ulTargetWidth = dstwidth;
	crt.ulTargetHeight = dstheight;
	crt.display_area.left = crt.display_area.top = 0;
	crt.display_area.right = iwidth;
	crt.display_area.bottom = iheight;
	crt.ulNumOutputSurfaces = 1;
	crt.ulCreationFlags = cudaVideoCreate_PreferCUVID;
	crt.vidLock = NULL;

	if(cuvidCreateDecoder(&nvp->decoder, &crt) != CUDA_SUCCESS) {
		ERR(dec, "decoder creation failed");
		return false;
	}

	if(dstwidth != nvp->d.wdst || dstheight != nvp->d.hdst) {
		if(nvp->rgb != 0) {
			if(cuMemFree(nvp->rgb) != CUDA_SUCCESS) {
				ERR(dec, "Could not free internal RGB buffer.");
				return false;
			}
			nvp->rgb = 0;
		}
		/* after decode, the buffer is in NV12 format.  A CUDA kernel
		 * reorganizes/converts to RGB, outputting into this buffer.  We will then
		 * do a standard CUDA copy to put it in the output buffer, since our API
		 * works completely on host memory for now. */
		const size_t nb_rgb = dstwidth*dstheight*3;
		if(cuMemAlloc(&nvp->rgb, nb_rgb) != CUDA_SUCCESS) {
			ERR(dec, "could not allocate temporary RGB buffer");
			nvp->rgb = 0;
			return false;
		}
		nvp->d.wdst = dstwidth;
		nvp->d.hdst = dstheight;
	}

	nvp->initialized = true;
	return true;
}

/* Resizes an existing decoder. */
void
resize(struct nvp_decoder* nvp, size_t width, size_t height, size_t dstwidth,
       size_t dstheight) {
	if(nvp->decoder && cuvidDestroyDecoder(nvp->decoder) != CUDA_SUCCESS) {
		ERR(dec, "Error destroying decoder");
	}
	nvp->decoder = NULL;
	dec_initialize(nvp, width, height, dstwidth, dstheight);
}

static void
nvp_cuvid_destroy(nvpipe* const __restrict cdc) {
	struct nvp_decoder* nvp = (struct nvp_decoder*)cdc;
	assert(nvp->impl.type == DECODER);

	if(nvp->decoder && cuvidDestroyDecoder(nvp->decoder) != CUDA_SUCCESS) {
		WARN(dec, "Error destroying decoder");
	}
	if(nvp->parser && cuvidDestroyVideoParser(nvp->parser) != CUDA_SUCCESS) {
		WARN(dec, "Error destroying parser.");
	}
	if(cuMemFree(nvp->rgb) != CUDA_SUCCESS) {
		WARN(dec, "Error freeing decode temporary buffer.");
	}
	if(nvp->reorg) {
		nvp->reorg->destroy(nvp->reorg);
		nvp->reorg = NULL;
	}
	free(nvp);
}

static int
dec_sequence(void* cdc, CUVIDEOFORMAT* fmt) {
	struct nvp_decoder* nvp = (struct nvp_decoder*)cdc;
	/* warn the user if the image is too large, but try it anyway. */
	if((size_t)fmt->display_area.right > MAX_WIDTH ||
	   (size_t)fmt->display_area.bottom > MAX_HEIGHT) {
		WARN(dec, "Video stream exceeds (%zux%zu) limits.", MAX_WIDTH, MAX_HEIGHT);
	}
	if(fmt->bit_depth_luma_minus8) {
		WARN(dec, "Unhandled bit depth (%d).  Was the frame compressed by "
		     "a different version of this library?", fmt->bit_depth_luma_minus8);
		return 0;
	}

	/* We could read the format from 'fmt' and then use that to
	 * (cuvid)Create-a-Decoder.  But since we know we're getting the results of
	 * NvPipe, we already know the stream type, and so we just specify
	 * explicitly. */
	assert(fmt->chroma_format == cudaVideoChromaFormat_420);
	assert(fmt->codec == cudaVideoCodec_H264);
	assert(fmt->progressive_sequence == 1);
	const size_t w = fmt->display_area.right - fmt->display_area.left;
	const size_t h = fmt->display_area.bottom - fmt->display_area.top;
	/* This appears to happen sometimes, which height are we supposed to use? */
	if(fmt->coded_height != h) {
		TRACE(dec, "coded height (%u) does not correspond to height (%zu).",
		      fmt->coded_height, h);
	}
	/* If this is our first sequence, both the decoder and our internal buffer
	 * need initializing. */
	if(!nvp->initialized) {
		if(!dec_initialize(nvp, w,h, w,h)) {
			return 0;
		}
	}
	return 1;
}

static int
dec_ode(void* cdc, CUVIDPICPARAMS* pic) {
	struct nvp_decoder* nvp = (struct nvp_decoder*)cdc;
	nvtxRangePush("cuvid DecodePicture");
	const CUresult dec = cuvidDecodePicture(nvp->decoder, pic);
	nvtxRangePop();
	if(CUDA_SUCCESS != dec) {
		WARN(dec, "Error %d decoding frame", dec);
		return 0;
	}
	/* Make sure this is after the decode+error check: we use it to figure out if
	 * this function executed successfully. */
	nvp->d.wsrc = pic->PicWidthInMbs*16;
	nvp->d.hsrc = pic->FrameHeightInMbs*16;
	return 1;
}

static nvp_err_t
initialize_parser(struct nvp_decoder* nvp) {
	CUVIDPARSERPARAMS prs = {0};
	prs.CodecType = cudaVideoCodec_H264;
	prs.ulMaxNumDecodeSurfaces = 1;
	prs.ulErrorThreshold = 100;
	/* when MaxDisplayDelay > 0, we can't assure that each input frame will be
	 * ready immediately.  If your application can tolerate frame latency, you
	 * might consider increasing this and introducing an EINTR-esque interface.
	 * Diminishing returns beyond 4. */
	prs.ulMaxDisplayDelay = 0;
	prs.pUserData = nvp;
	prs.pfnSequenceCallback = dec_sequence;
	prs.pfnDecodePicture = dec_ode;
	prs.pfnDisplayPicture = NULL;
	if(cuvidCreateVideoParser(&nvp->parser, &prs) != CUDA_SUCCESS) {
		ERR(dec, "failed creating video parser.");
		return NVPIPE_EDECODE;
	}
	return NVPIPE_SUCCESS;
}

/** decode/decompress packets
 *
 * Decode a frame into the given buffer.
 *
 * @param[in] codec instance variable
 * @param[in] ibuf the compressed frame
 * @param[in] ibuf_sz  the size in bytes of the compressed data
 * @param[out] obuf where the output frame will be written. must be w*h*3 bytes.
 * @param[in] width width of output image
 * @param[in] height height of output image
 *
 * @return NVPIPE_SUCCESS on success, nonzero on error.
 */
nvp_err_t
nvp_cuvid_decode(nvpipe* const cdc,
                 const void* const __restrict ibuf,
                 const size_t ibuf_sz,
                 void* const __restrict obuf,
                 size_t width, size_t height) {
	struct nvp_decoder* nvp = (struct nvp_decoder*)cdc;
	if(nvp->impl.type != DECODER) {
		ERR(dec, "backend implementation configuration error");
		return NVPIPE_EINVAL;
	}
	if(ibuf_sz == 0) {
		ERR(dec, "input buffer size is 0.");
		return NVPIPE_EINVAL;
	}
	if(width == 0 || height == 0 || (height&0x1) == 1) {
		ERR(dec, "invalid width or height");
		return NVPIPE_EINVAL;
	}

	if(NULL == nvp->parser) { /* i.e. the first frame */
		const nvp_err_t errc = initialize_parser(nvp);
		if(errc) {
			return errc;
		}
	}

	CUVIDSOURCEDATAPACKET pkt = {0};
	pkt.payload_size = ibuf_sz;
	pkt.payload = ibuf;
	nvtxRangePush("parse video data");
	const CUresult parse = cuvidParseVideoData(nvp->parser, &pkt);
	nvtxRangePop();
	if(CUDA_SUCCESS != parse) {
		ERR(dec, "parsing video data failed");
		return NVPIPE_EDECODE;
	}	/* That fired off all our dec_* callbacks. */

	if(nvp->d.wsrc == 0 || nvp->d.hsrc == 0) {
		/* A frame of latency means cuvid doesn't always fire our callbacks.  So,
		 * just resubmit the frame again, but do a quick check to make sure we do
		 * not recurse endlessly. */
		if(nvp->empty) {
			ERR(dec, "Input is just stream metadata!");
			return NVPIPE_EINVAL;
		}
		nvp->empty = true;
		return nvp_cuvid_decode(cdc, ibuf, ibuf_sz, obuf, width, height);
	}
	nvp->empty = false;

	/* 4 cases: sizes are unchanged; size to scale to changed; input image size
	 * changed; both input image size and size to scale to changed.  We check for
	 * buffer size differences in 'resize', though, so they all boil down to:
	 * resize(), and then call ourself again (i.e. resubmit the frame).
	 * One could optimize the scaling cases by potentially reusing the buffer,
	 * technically. */
	if(nvp->d.wsrc != nvp->d.wi || nvp->d.hsrc != nvp->d.hi ||
	   nvp->d.wdst != width || nvp->d.hdst != height) {
		resize(nvp, nvp->d.wsrc, nvp->d.hsrc, width, height);
		return nvp_cuvid_decode(cdc, ibuf, ibuf_sz, obuf, width, height);
	}

	CUVIDPROCPARAMS map = {0};
	map.progressive_frame = 1;
	unsigned pitch = 0;
	CUdeviceptr data = 0;
	const unsigned pic = 0;
	assert(nvp->decoder);
	nvtxRangePush("map frame");
	CUresult mrs = cuvidMapVideoFrame(nvp->decoder, pic,
	                                  (unsigned long long*)&data, &pitch, &map);
	nvtxRangePop();
	if(CUDA_SUCCESS != mrs) {
		ERR(dec, "Failed mapping frame: %d", mrs);
		return mrs;
	}

	if(NULL == nvp->reorg) {
		nvp->reorg = nv122rgb();
	}
	nvp_err_t errcode = NVPIPE_SUCCESS;
	nvtxRangePush("reorganize and copy");
	/* reformat 'data' into 'nvp->rgb'. Both are device memory */
	const CUresult sub = nvp->reorg->submit(nvp->reorg, data, width, height,
	                                        nvp->rgb, pitch);
	if(CUDA_SUCCESS != sub) {
		nvtxRangePop();
		errcode = sub;
		goto fail;
	}
	/* copy the result into the user's buffer. */
	const size_t nb_rgb = nvp->d.wdst*nvp->d.hdst*3;
	const CUresult hcopy = cuMemcpyDtoHAsync(obuf, nvp->rgb, nb_rgb,
	                                         nvp->reorg->strm);
	if(CUDA_SUCCESS != hcopy) {
		nvtxRangePop();
		errcode = hcopy;
		goto fail;
	}
	const CUresult synch = nvp->reorg->sync(nvp->reorg);
	if(CUDA_SUCCESS != synch) {
		nvtxRangePop();
		errcode = synch;
		goto fail;
	}
	nvtxRangePop();

fail:
	if(cuvidUnmapVideoFrame(nvp->decoder, data) != CUDA_SUCCESS) {
		WARN(dec, "Could not unmap frame.");
	}

	return errcode;
}

/* The decoder can't encode.  Just error and bail. */
static nvp_err_t
nvp_cuvid_encode(nvpipe * const __restrict codec,
                 const void *const __restrict ibuf,
                 const size_t ibuf_sz,
                 void *const __restrict obuf,
                 size_t* const __restrict obuf_sz,
                 const size_t width, const size_t height,
                 nvp_fmt_t format) {
	(void)codec; (void)ibuf; (void)ibuf_sz;
	(void)obuf; (void)obuf_sz;
	(void)width; (void)height;
	(void)format;
	ERR(dec, "Decoder cannot encode; create an encoder instead.");
	assert(false); /* Such use always indicates a programmer error. */
	return NVPIPE_EINVAL;
}

static nvp_err_t
nvp_cuvid_bitrate(nvpipe* codec, uint64_t br) {
	(void)codec; (void)br;
	ERR(dec, "Bitrate is encoded into the stream; you can only change it"
	    " on the encode side.");
	assert(false); /* Such use always indicates a programmer error. */
	return NVPIPE_EINVAL;
}

nvp_impl_t*
nvp_create_decoder() {
	struct nvp_decoder* nvp = calloc(1, sizeof(struct nvp_decoder));
	nvp->impl.type = DECODER;
	nvp->impl.encode = nvp_cuvid_encode;
	nvp->impl.bitrate = nvp_cuvid_bitrate;
	nvp->impl.decode = nvp_cuvid_decode;
	nvp->impl.destroy = nvp_cuvid_destroy;

	/* Ensure the runtime API initializes its implicit context. */
	cudaDeviceSynchronize();

	return (nvp_impl_t*)nvp;
}
