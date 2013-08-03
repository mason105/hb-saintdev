/* ********************************************************************* *\

Copyright (C) 2013 Intel Corporation.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
- Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.
- Neither the name of Intel Corporation nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY INTEL CORPORATION "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL INTEL CORPORATION BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

\* ********************************************************************* */

#include <stdarg.h>
#include "hb.h"
#include "enc_qsv.h"
#include "qsv_common.h"
#include "h264_common.h"
#include "qsv_filter_pp.h"

int  encqsvInit( hb_work_object_t *, hb_job_t * );
int  encqsvWork( hb_work_object_t *, hb_buffer_t **, hb_buffer_t ** );
void encqsvClose( hb_work_object_t * );

hb_work_object_t hb_encqsv =
{
    WORK_ENCQSV,
    "H.264/AVC encoder (Intel QSV)",
    encqsvInit,
    encqsvWork,
    encqsvClose
};

#define SPSPPS_SIZE     1024

struct hb_work_private_s
{
    hb_job_t       *job;
    hb_esconfig_t  *config;
    uint32_t       frames_in;
    uint32_t       frames_out;
    int64_t        last_start;

    hb_qsv_param_t param;

    mfxEncodeCtrl *force_keyframe;
    struct
    {
        int     index;
        int64_t start;
    } next_chapter;

#define BFRM_DELAY_MAX 16
    // for DTS generation (when MSDK API < 1.6 or VFR)
    int            bfrm_delay;
    int            bfrm_workaround;
    int64_t        init_pts[BFRM_DELAY_MAX + 1];
    hb_list_t     *list_dts;

    hb_list_t*  delayed_processing;

    int async_depth;
    int max_async_depth;

    // is only encode/system memory used
    int is_sys_mem;
    struct SwsContext* sws_context_to_nv12;

    // is to expect VPP before
    int is_vpp_present;

    // whether the encoder is initialized
    int init_done;
};

// for DTS generation (when MSDK API < 1.6 or VFR)
static void hb_qsv_add_new_dts(hb_list_t *list, int64_t new_dts)
{
    if (list != NULL)
    {
        int64_t *item = malloc(sizeof(int64_t));
        if (item != NULL)
        {
            *item = new_dts;
            hb_list_add(list, item);
        }
    }
}
static int64_t hb_qsv_pop_next_dts(hb_list_t *list)
{
    int64_t next_dts = INT64_MIN;
    if (list != NULL && hb_list_count(list) > 0)
    {
        int64_t *item = hb_list_item(list, 0);
        if (item != NULL)
        {
            next_dts = *item;
            hb_list_rem(list, item);
            free(item);
        }
    }
    return next_dts;
}

int qsv_enc_init(av_qsv_context *qsv, hb_work_private_t *pv)
{
    int i = 0;
    mfxStatus sts;
    hb_job_t *job = pv->job;

    if (pv->init_done)
    {
        return 0;
    }
    
    pv->is_sys_mem = !hb_qsv_decode_is_enabled(job);
    if (qsv == NULL)
    {
        if (!pv->is_sys_mem)
        {
            hb_error("qsv_enc_init: decode enabled but no context!");
            return 3;
        }
        qsv = av_mallocz(sizeof(av_qsv_context));
    }
    
    av_qsv_space *qsv_encode = qsv->enc_space;
    if (qsv_encode == NULL)
    {
        qsv_encode = av_mallocz(sizeof(av_qsv_space));
        // if only for encode
        if (pv->is_sys_mem)
        {
            memset(&qsv->mfx_session, 0, sizeof(mfxSession));
            qsv->ver.Major = AV_QSV_MSDK_VERSION_MAJOR;
            qsv->ver.Minor = AV_QSV_MSDK_VERSION_MINOR;
            qsv->impl      = MFX_IMPL_AUTO_ANY;
            sts = MFXInit(qsv->impl, &qsv->ver, &qsv->mfx_session);
            AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

            // no need to use additional sync as encode only -> single thread
            av_qsv_add_context_usage(qsv, 0);
            job->qsv = qsv;
        }
        else
        {
            av_qsv_add_context_usage(qsv, HAVE_THREADS);
        }
        qsv->enc_space = qsv_encode;
    }
    if (qsv_encode->is_init_done)
    {
        return 0;
    }
    
    if (!pv->is_sys_mem)
    {
        if (!pv->is_vpp_present && job->list_filter != NULL)
        {
            for (i = 0; i < hb_list_count(job->list_filter); i++)
            {
                hb_filter_object_t *filter = hb_list_item(job->list_filter, i);
                if (filter->id == HB_FILTER_QSV_PRE  ||
                    filter->id == HB_FILTER_QSV_POST ||
                    filter->id == HB_FILTER_QSV)
                {
                    pv->is_vpp_present = 1;
                    break;
                }
            }
        }
        
        if (pv->is_vpp_present)
        {
            if (qsv->vpp_space == NULL)
            {
                return 2;
            }
            for (i = 0; i < av_qsv_list_count(qsv->vpp_space); i++)
            {
                av_qsv_space *vpp = av_qsv_list_item(qsv->vpp_space, i);
                if (!vpp->is_init_done)
                {
                    return 2;
                }
            }
        }

        av_qsv_space *dec_space = qsv->dec_space;
        if (dec_space == NULL || !dec_space->is_init_done)
        {
            return 2;
        }
    }
    else
    {
        pv->sws_context_to_nv12 = hb_sws_get_context(job->width, job->height,
                                                     AV_PIX_FMT_YUV420P,
                                                     job->width, job->height,
                                                     AV_PIX_FMT_NV12,
                                                     SWS_LANCZOS|SWS_ACCURATE_RND);
    }

    // if we don't know how many tasks we may have, make it at least one
    int tasks_amount           = pv->max_async_depth ? pv->max_async_depth : 1;
    qsv_encode->tasks          = av_qsv_list_init(HAVE_THREADS);
    qsv_encode->p_buf_max_size = AV_QSV_BUF_SIZE_DEFAULT;

    for (i = 0; i < tasks_amount; i++)
    {
        av_qsv_task *task    = av_mallocz(sizeof(av_qsv_task));
        task->bs             = av_mallocz(sizeof(mfxBitstream));
        task->bs->Data       = av_mallocz(sizeof(uint8_t) * qsv_encode->p_buf_max_size);
        task->bs->MaxLength  = qsv_encode->p_buf_max_size;
        task->bs->DataLength = 0;
        task->bs->DataOffset = 0;
        av_qsv_list_add(qsv_encode->tasks, task);
    }

    // setup surface allocation
    memset(&qsv_encode->request, 0, sizeof(mfxFrameAllocRequest) * 2);
    pv->param.videoParam.IOPattern = (pv->is_sys_mem ?
                                      MFX_IOPATTERN_IN_SYSTEM_MEMORY :
                                      MFX_IOPATTERN_IN_OPAQUE_MEMORY);
    sts = MFXVideoENCODE_QueryIOSurf(qsv->mfx_session,
                                     &pv->param.videoParam,
                                     &qsv_encode->request);
    AV_QSV_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
    AV_QSV_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    // allocate surfaces
    if (pv->is_sys_mem)
    {
        qsv_encode->surface_num = FFMIN(qsv_encode->request[0].NumFrameSuggested +
                                        pv->job->qsv_async_depth, AV_QSV_SURFACE_NUM);
        if (qsv_encode->surface_num <= 0)
        {
            qsv_encode->surface_num = AV_QSV_SURFACE_NUM;
        }
        for (i = 0; i < qsv_encode->surface_num; i++)
        {
            qsv_encode->p_surfaces[i] = av_mallocz(sizeof(mfxFrameSurface1));
            AV_QSV_CHECK_POINTER(qsv_encode->p_surfaces[i], MFX_ERR_MEMORY_ALLOC);
            memcpy(&(qsv_encode->p_surfaces[i]->Info),
                   &(qsv_encode->request[0].Info), sizeof(mfxFrameInfo));
        }
    }
    else
    {
        av_qsv_space *in_space = qsv->dec_space;
        if (pv->is_vpp_present)
        {
            // we get our input from VPP instead
            in_space = av_qsv_list_item(qsv->vpp_space,
                                        av_qsv_list_count(qsv->vpp_space) - 1);
        }
        // introduced in API 1.3
        memset(&qsv_encode->ext_opaque_alloc, 0, sizeof(mfxExtOpaqueSurfaceAlloc));
        qsv_encode->ext_opaque_alloc.Header.BufferId = MFX_EXTBUFF_OPAQUE_SURFACE_ALLOCATION;
        qsv_encode->ext_opaque_alloc.Header.BufferSz = sizeof(mfxExtOpaqueSurfaceAlloc);
        qsv_encode->ext_opaque_alloc.In.Surfaces     = in_space->p_surfaces;
        qsv_encode->ext_opaque_alloc.In.NumSurface   = in_space->surface_num;
        qsv_encode->ext_opaque_alloc.In.Type         = qsv_encode->request[0].Type;
        pv->param.videoParam.ExtParam[pv->param.videoParam.NumExtParam++] = (mfxExtBuffer*)&qsv_encode->ext_opaque_alloc;
    }

    // allocate sync points
    qsv_encode->sync_num = (qsv_encode->surface_num ?
                            FFMIN(qsv_encode->surface_num, AV_QSV_SYNC_NUM) :
                            AV_QSV_SYNC_NUM);
    for (i = 0; i < qsv_encode->sync_num; i++)
    {
        qsv_encode->p_syncp[i] = av_mallocz(sizeof(av_qsv_sync));
        AV_QSV_CHECK_POINTER(qsv_encode->p_syncp[i], MFX_ERR_MEMORY_ALLOC);
        qsv_encode->p_syncp[i]->p_sync = av_mallocz(sizeof(mfxSyncPoint));
        AV_QSV_CHECK_POINTER(qsv_encode->p_syncp[i]->p_sync, MFX_ERR_MEMORY_ALLOC);
    }

    sts = MFXVideoENCODE_Init(qsv->mfx_session, &pv->param.videoParam);
    AV_QSV_IGNORE_MFX_STS(sts, MFX_WRN_INCOMPATIBLE_VIDEO_PARAM);
    AV_QSV_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);
    qsv_encode->is_init_done = 1;

    if (pv->is_sys_mem)
    {
        hb_log("qsv_enc_init: using encode-only path");
    }
    if (MFXQueryIMPL(qsv->mfx_session, &qsv->impl) == MFX_ERR_NONE)
    {
        hb_log("qsv_enc_init: using Intel Media SDK %s implementation",
               qsv->impl == MFX_IMPL_SOFTWARE ? "software" : "hardware");
    }

    pv->init_done = 1;
    return 0;
}

/***********************************************************************
 * encqsvInit
 ***********************************************************************
 *
 **********************************************************************/
int encqsvInit(hb_work_object_t *w, hb_job_t *job)
{
    hb_work_private_t *pv = calloc(1, sizeof(hb_work_private_t));
    w->private_data       = pv;

    pv->job                = job;
    pv->delayed_processing = hb_list_init();
    pv->last_start         = INT64_MIN;
    pv->frames_in          = 0;
    pv->frames_out         = 0;
    pv->init_done          = 0;
    pv->is_sys_mem         = 0;
    pv->is_vpp_present     = 0;

    // set up a re-usable mfxEncodeCtrl to force keyframes (e.g. for chapters)
    pv->force_keyframe = calloc(1, sizeof(mfxEncodeCtrl));
    if (pv->force_keyframe == NULL)
    {
        hb_error("encqsvInit: mfxEncodeCtrl allocation failure");
        return -1;
    }
    pv->force_keyframe->QP          = 0;
    pv->force_keyframe->FrameType   = MFX_FRAMETYPE_I|MFX_FRAMETYPE_IDR|MFX_FRAMETYPE_REF;
    pv->force_keyframe->NumExtParam = 0;
    pv->force_keyframe->NumPayload  = 0;
    pv->force_keyframe->ExtParam    = NULL;
    pv->force_keyframe->Payload     = NULL;

    pv->next_chapter.index = 0;
    pv->next_chapter.start = INT64_MIN;

    // set encoding settings
    hb_qsv_param_default(&pv->param);
    if (hb_qsv_h264_param_init_for_job(&pv->param, job))
    {
        hb_error("encqsvInit: hb_qsv_h264_param_init_for_job failed");
        return -1;
    }

    // set the Asynd Depth to match that of decode and VPP
    pv->param.videoParam.AsyncDepth = job->qsv_async_depth;
    pv->max_async_depth             = job->qsv_async_depth;
    pv->async_depth                 = 0;

    // get the SPS/PPS
    mfxStatus err;
    mfxVersion version;
    mfxVideoParam videoParam;
    mfxSession session = (mfxSession)0;
    mfxExtCodingOptionSPSPPS sps_pps_buf, *sps_pps = &sps_pps_buf;
    version.Major = HB_QSV_MINVERSION_MAJOR;
    version.Minor = HB_QSV_MINVERSION_MINOR;
    err = MFXInit(MFX_IMPL_AUTO_ANY, &version, &session);
    if (err != MFX_ERR_NONE)
    {
        return err;
    }
    err = MFXVideoENCODE_Init(session, &pv->param.videoParam);
    if (err != MFX_ERR_NONE &&
        err != MFX_WRN_PARTIAL_ACCELERATION &&
        err != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM)
    {
        MFXClose(session);
        return err;
    }
    memset(&videoParam, 0, sizeof(mfxVideoParam));
    sps_pps->Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
    sps_pps->Header.BufferSz = sizeof(mfxExtCodingOptionSPSPPS);
    sps_pps->SPSId           = 0;
    sps_pps->SPSBuffer       = w->config->h264.sps;
    sps_pps->SPSBufSize      = sizeof(w->config->h264.sps);
    sps_pps->PPSId           = 0;
    sps_pps->PPSBuffer       = w->config->h264.pps;
    sps_pps->PPSBufSize      = sizeof(w->config->h264.pps);
    videoParam.NumExtParam   = 1;
    videoParam.ExtParam      = (mfxExtBuffer**)&sps_pps;
    err = MFXVideoENCODE_GetVideoParam(session, &videoParam);
    MFXVideoENCODE_Close(session);
    MFXClose(session);
    if (err == MFX_ERR_NONE)
    {
        // remove 32-bit NAL prefix (0x00 0x00 0x00 0x01)
        w->config->h264.sps_length = sps_pps->SPSBufSize - 4;
        memmove(w->config->h264.sps, w->config->h264.sps + 4,
                w->config->h264.sps_length);
        w->config->h264.pps_length = sps_pps->PPSBufSize - 4;
        memmove(w->config->h264.pps, w->config->h264.pps + 4,
                w->config->h264.pps_length);
    }
    else
    {
        w->config->h264.sps_length = w->config->h264.pps_length = 0;
    }

    // check whether B-frames are used
    switch (videoParam.mfx.CodecProfile)
    {
        case MFX_PROFILE_AVC_BASELINE:
        case MFX_PROFILE_AVC_CONSTRAINED_HIGH:
        case MFX_PROFILE_AVC_CONSTRAINED_BASELINE:
            pv->bfrm_delay = 0;
            break;
        default:
            pv->bfrm_delay = 1;
            break;
    }
    if (videoParam.mfx.GopRefDist > 0)
    {
        pv->bfrm_delay = FFMIN(pv->bfrm_delay, videoParam.mfx.GopRefDist - 1);
    }
    if (videoParam.mfx.GopPicSize > 0)
    {
        pv->bfrm_delay = FFMIN(pv->bfrm_delay, videoParam.mfx.GopPicSize - 2);
    }
    // sanitize
    pv->bfrm_delay = FFMAX(pv->bfrm_delay, 0);
    // check whether we need to generate DTS ourselves (MSDK API < 1.6 or VFR)
    pv->bfrm_workaround = job->cfr != 1 || !(hb_qsv_info->capabilities &
                                             HB_QSV_CAP_MSDK_API_1_6);
    if (pv->bfrm_delay && pv->bfrm_workaround)
    {
        pv->bfrm_workaround = 1;
        pv->list_dts        = hb_list_init();
    }
    else
    {
        pv->bfrm_workaround = 0;
        pv->list_dts        = NULL;
    }

    // let the muxer know whether to expect B-frames or not
    job->areBframes = !!pv->bfrm_delay;

    // log principal output settings
    switch (videoParam.mfx.RateControlMethod)
    {
        case MFX_RATECONTROL_LA:
            hb_log("encqsvInit: MFX_RATECONTROL_LA with TargetKbps %"PRIu16"",
                   videoParam.mfx.TargetKbps);
            break;
        case MFX_RATECONTROL_AVBR:
            hb_log("encqsvInit: MFX_RATECONTROL_AVBR with TargetKbps %"PRIu16"",
                   videoParam.mfx.TargetKbps);
            break;
        case MFX_RATECONTROL_CQP:
            hb_log("encqsvInit: MFX_RATECONTROL_CQP with QPI %"PRIu16", QPP %"PRIu16", QPB %"PRIu16"",
                   videoParam.mfx.QPI, videoParam.mfx.QPP, videoParam.mfx.QPB);
            break;
        case MFX_RATECONTROL_CBR:
        case MFX_RATECONTROL_VBR:
            hb_log("encqsvInit: MFX_RATECONTROL_%s with TargetKbps %"PRIu16", MaxKbps %"PRIu16"",
                   videoParam.mfx.RateControlMethod == MFX_RATECONTROL_CBR ? "CBR" : "VBR",
                   videoParam.mfx.TargetKbps, videoParam.mfx.MaxKbps);
            hb_log("encqsvInit: VBV enabled with BufferSizeInKB %"PRIu16" and InitialDelayInKB %"PRIu16"",
                   videoParam.mfx.BufferSizeInKB, videoParam.mfx.InitialDelayInKB);
            break;
        default:
            hb_log("encqsvInit: invalid rate control method %"PRIu16"",
                   videoParam.mfx.RateControlMethod);
            return -1;
    }
    hb_log("encqsvInit: GopRefDist %"PRIu16" GopPicSize %"PRIu16" NumRefFrame %"PRIu16"",
           videoParam.mfx.GopRefDist, videoParam.mfx.GopPicSize, videoParam.mfx.NumRefFrame);
    hb_log("encqsvInit: TargetUsage %"PRIu16" AsyncDepth %"PRIu16"",
           videoParam.mfx.TargetUsage, videoParam.AsyncDepth);
    hb_log("encqsvInit: H.264 Profile %"PRIu16" H.264 Level %"PRIu16"",
           videoParam.mfx.CodecProfile, videoParam.mfx.CodecLevel);

    return 0;
}

void encqsvClose( hb_work_object_t * w )
{
    int i = 0;
    hb_work_private_t * pv = w->private_data;

    hb_log( "enc_qsv done: frames: %u in, %u out", pv->frames_in, pv->frames_out );

    // if system memory ( encode only ) additional free(s) for surfaces
    if( pv && pv->job && pv->job->qsv &&
        pv->job->qsv->is_context_active ){

        av_qsv_context *qsv = pv->job->qsv;

        if(qsv && qsv->enc_space){
        av_qsv_space* qsv_encode = qsv->enc_space;
        if(qsv_encode->is_init_done){
            if(pv->is_sys_mem){
                if( qsv_encode && qsv_encode->surface_num > 0)
                    for (i = 0; i < qsv_encode->surface_num; i++){
                        if( qsv_encode->p_surfaces[i]->Data.Y){
                            free(qsv_encode->p_surfaces[i]->Data.Y);
                            qsv_encode->p_surfaces[i]->Data.Y = 0;
                        }
                        if( qsv_encode->p_surfaces[i]->Data.VU){
                            free(qsv_encode->p_surfaces[i]->Data.VU);
                            qsv_encode->p_surfaces[i]->Data.VU = 0;
                        }
                        if(qsv_encode->p_surfaces[i])
                            av_freep(qsv_encode->p_surfaces[i]);
                    }
                qsv_encode->surface_num = 0;

                sws_freeContext(pv->sws_context_to_nv12);
            }

            for (i = av_qsv_list_count(qsv_encode->tasks); i > 1; i--){
                av_qsv_task* task = av_qsv_list_item(qsv_encode->tasks,i-1);
                if(task && task->bs){
                    av_freep(&task->bs->Data);
                    av_freep(&task->bs);
                    av_qsv_list_rem(qsv_encode->tasks,task);
                }
            }
            av_qsv_list_close(&qsv_encode->tasks);

            for (i = 0; i < qsv_encode->surface_num; i++){
                av_freep(&qsv_encode->p_surfaces[i]);
            }
            qsv_encode->surface_num = 0;

                if( qsv_encode->p_ext_param_num || qsv_encode->p_ext_params )
                    av_freep(&qsv_encode->p_ext_params);

            for (i = 0; i < qsv_encode->sync_num; i++){
                    av_freep(&qsv_encode->p_syncp[i]->p_sync);
                    av_freep(&qsv_encode->p_syncp[i]);
            }
            qsv_encode->sync_num = 0;

                qsv_encode->is_init_done = 0;
        }
        }
        if(qsv->enc_space)
        av_freep(&qsv->enc_space);

        if(qsv){
        // closing the commong stuff
        av_qsv_context_clean(qsv);

        if(pv->is_sys_mem){
            av_freep(&qsv);
        }
        }
    }

    if (pv != NULL)
    {
        if (pv->list_dts != NULL)
        {
            while (hb_list_count(pv->list_dts) > 0)
            {
                int64_t *item = hb_list_item(pv->list_dts, 0);
                hb_list_rem(pv->list_dts, item);
                free(item);
            }
            hb_list_close(&pv->list_dts);
        }
        if (pv->force_keyframe != NULL)
        {
            free(pv->force_keyframe);
            pv->force_keyframe = NULL;
        }
    }

    free( pv );
    w->private_data = NULL;
}

int encqsvWork( hb_work_object_t * w, hb_buffer_t ** buf_in,
                  hb_buffer_t ** buf_out )
{
    hb_work_private_t * pv = w->private_data;
    hb_job_t * job = pv->job;
    hb_buffer_t * in = *buf_in, *buf;
    av_qsv_context *qsv = job->qsv;
    av_qsv_space* qsv_encode;
    hb_buffer_t *last_buf = NULL;
    mfxStatus sts = MFX_ERR_NONE;
    int is_end = 0;
    av_qsv_list* received_item = 0;
    av_qsv_stage* stage = 0;

    while(1){
        int ret = qsv_enc_init(qsv, pv);
        qsv = job->qsv;
        qsv_encode = qsv->enc_space;
        if(ret >= 2)
            av_qsv_sleep(1);
        else
            break;
    }
    *buf_out = NULL;

    if( in->size <= 0 )
    {
        // do delayed frames yet
        *buf_in = NULL;
        is_end = 1;
    }

    // input from decode, as called - we always have some to proceed with
    while( 1 ){

    {
        mfxEncodeCtrl    *work_control = NULL;
        mfxFrameSurface1 *work_surface = NULL;

        if(!is_end) {
            if(pv->is_sys_mem){
                int surface_idx = av_qsv_get_free_surface(qsv_encode, qsv, &qsv_encode->request[0].Info, QSV_PART_ANY);
                work_surface = qsv_encode->p_surfaces[surface_idx];

                if(!work_surface->Data.Y){
                    // if nv12 and 422 12bits per pixel
                    work_surface->Data.Y  = calloc(1,
                                                   pv->param.videoParam.mfx.FrameInfo.Width *
                                                   pv->param.videoParam.mfx.FrameInfo.Height);
                    work_surface->Data.VU = calloc(1,
                                                   pv->param.videoParam.mfx.FrameInfo.Width *
                                                   pv->param.videoParam.mfx.FrameInfo.Height / 2);
                    work_surface->Data.Pitch = pv->param.videoParam.mfx.FrameInfo.Width;
                }
                qsv_yuv420_to_nv12(pv->sws_context_to_nv12,work_surface,in);
            }
            else{
                received_item = in->qsv_details.qsv_atom;
                stage = av_qsv_get_last_stage( received_item );
                work_surface = stage->out.p_surface;
            }

            work_surface->Data.TimeStamp = in->s.start;

            /*
             * Debugging code to check that the upstream modules have generated
             * a continuous, self-consistent frame stream.
             */
            int64_t start = work_surface->Data.TimeStamp;
            if (pv->last_start > start)
            {
                hb_log("encqsvWork: input continuity error, last start %"PRId64" start %"PRId64"",
                       pv->last_start, start);
            }
            pv->last_start = start;

            // for DTS generation (when MSDK API < 1.6 or VFR)
            if (pv->bfrm_delay && pv->bfrm_workaround)
            {
                if (pv->frames_in <= BFRM_DELAY_MAX)
                {
                    pv->init_pts[pv->frames_in] = work_surface->Data.TimeStamp;
                }
                if (pv->frames_in)
                {
                    hb_qsv_add_new_dts(pv->list_dts,
                                       work_surface->Data.TimeStamp);
                }
            }

            /*
             * Chapters have to start with a keyframe so request that this
             * frame be coded as IDR. Since there may be several frames
             * buffered in the encoder, remember the timestamp so when this
             * frame finally pops out of the encoder we'll mark its buffer
             * as the start of a chapter.
             */
            if (in->s.new_chap > 0 && job->chapter_markers)
            {
                if (!pv->next_chapter.index)
                {
                    pv->next_chapter.start = work_surface->Data.TimeStamp;
                    pv->next_chapter.index = in->s.new_chap;
                    work_control           = pv->force_keyframe;
                }
                else
                {
                    // however unlikely, this can happen in theory
                    hb_log("encqsvWork: got chapter %d before we could write chapter %d, dropping marker",
                           in->s.new_chap, pv->next_chapter.index);
                }
                // don't let 'work_loop' put a chapter mark on the wrong buffer
                in->s.new_chap = 0;
            }
        }
        else{
            work_surface = NULL;
            received_item = NULL;
        }
        int sync_idx = av_qsv_get_free_sync( qsv_encode, qsv );
        if (sync_idx == -1)
        {
            hb_error("qsv: Not enough resources allocated for QSV encode");
            return 0;
        }
        av_qsv_task *task = av_qsv_list_item( qsv_encode->tasks, pv->async_depth );

        for (;;)
        {
            // Encode a frame asychronously (returns immediately)
            sts = MFXVideoENCODE_EncodeFrameAsync(qsv->mfx_session,
                                                  work_control, work_surface, task->bs,
                                                  qsv_encode->p_syncp[sync_idx]->p_sync);

            if (MFX_ERR_MORE_DATA == sts || (MFX_ERR_NONE <= sts && MFX_WRN_DEVICE_BUSY != sts))
                if (work_surface && !pv->is_sys_mem)
                    ff_qsv_atomic_dec(&work_surface->Data.Locked);

            if( MFX_ERR_MORE_DATA == sts ){
                ff_qsv_atomic_dec(&qsv_encode->p_syncp[sync_idx]->in_use);
                if(work_surface && received_item)
                    hb_list_add(pv->delayed_processing, received_item);
                break;
            }

            AV_QSV_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

            if (MFX_ERR_NONE <= sts /*&& !syncpE*/) // repeat the call if warning and no output
            {
                if (MFX_WRN_DEVICE_BUSY == sts){
                    av_qsv_sleep(10); // wait if device is busy
                    continue;
                }

                av_qsv_stage* new_stage = av_qsv_stage_init();
                new_stage->type = AV_QSV_ENCODE;
                new_stage->in.p_surface = work_surface;
                new_stage->out.sync = qsv_encode->p_syncp[sync_idx];

                new_stage->out.p_bs = task->bs;//qsv_encode->bs;
                task->stage = new_stage;

                pv->async_depth++;

                if(received_item){
                    av_qsv_add_stagee( &received_item, new_stage,HAVE_THREADS );
                }
                else{
                   // flushing the end
                    int pipe_idx = av_qsv_list_add( qsv->pipes, av_qsv_list_init(HAVE_THREADS) );
                    av_qsv_list* list_item = av_qsv_list_item( qsv->pipes, pipe_idx );
                    av_qsv_add_stagee( &list_item, new_stage,HAVE_THREADS );
                }

                int i = 0;
                for(i=hb_list_count(pv->delayed_processing); i > 0;i--){
                    hb_list_t *item = hb_list_item(pv->delayed_processing,i-1);
                    if(item){
                        hb_list_rem(pv->delayed_processing,item);
                        av_qsv_flush_stages(qsv->pipes, &item);
                    }
                }

                break;
            }

            ff_qsv_atomic_dec(&qsv_encode->p_syncp[sync_idx]->in_use);

            if (MFX_ERR_NOT_ENOUGH_BUFFER == sts)
                DEBUG_ASSERT( 1,"The bitstream buffer size is insufficient." );

            break;
        }
    }

    buf = NULL;

    do{

    if(pv->async_depth==0) break;

    // working properly with sync depth approach of MediaSDK OR flushing, if at the end
    if( (pv->async_depth >= pv->max_async_depth) || is_end ){

        pv->async_depth--;

        av_qsv_task *task = av_qsv_list_item( qsv_encode->tasks, 0 );
        av_qsv_stage* stage = task->stage;
        av_qsv_list*  this_pipe = av_qsv_pipe_by_stage(qsv->pipes,stage);
        sts = MFX_ERR_NONE;

        // only here we need to wait on operation been completed, therefore SyncOperation is used,
        // after this step - we continue to work with bitstream, muxing ...
        av_qsv_wait_on_sync( qsv,stage );

        if(task->bs->DataLength>0){
                av_qsv_flush_stages( qsv->pipes, &this_pipe );

                // see nal_encode
                buf = hb_video_buffer_init( job->width, job->height );
                buf->size = 0;
                buf->s.frametype = 0;

                // maping of FrameType(s)
                if(task->bs->FrameType & MFX_FRAMETYPE_IDR ) buf->s.frametype = HB_FRAME_IDR;
                else
                if(task->bs->FrameType & MFX_FRAMETYPE_I )   buf->s.frametype = HB_FRAME_I;
                else
                if(task->bs->FrameType & MFX_FRAMETYPE_P )   buf->s.frametype = HB_FRAME_P;
                else
                if(task->bs->FrameType & MFX_FRAMETYPE_B )   buf->s.frametype = HB_FRAME_B;

                if(task->bs->FrameType & MFX_FRAMETYPE_REF ) buf->s.flags      = HB_FRAME_REF;

                parse_nalus(task->bs->Data + task->bs->DataOffset,task->bs->DataLength, buf, pv->frames_out);

                if ( last_buf == NULL )
                    *buf_out = buf;
                else
                    last_buf->next = buf;
                last_buf = buf;

            // simple for now but check on TimeStampCalc from MSDK
            int64_t duration  = ((double)pv->param.videoParam.mfx.FrameInfo.FrameRateExtD /
                                 (double)pv->param.videoParam.mfx.FrameInfo.FrameRateExtN) * 90000.;

            // start        -> PTS
            // renderOffset -> DTS
            buf->s.start    = buf->s.renderOffset = task->bs->TimeStamp;
            buf->s.stop     = buf->s.start + duration;
            buf->s.duration = duration;
            if (pv->bfrm_delay)
            {
                if (!pv->bfrm_workaround)
                {
                    buf->s.renderOffset = task->bs->DecodeTimeStamp;
                }
                else
                {
                    // MSDK API < 1.6 or VFR, so generate our own DTS
                    if ((pv->frames_out == 0)                                 &&
                        (hb_qsv_info->capabilities & HB_QSV_CAP_MSDK_API_1_6) &&
                        (hb_qsv_info->capabilities & HB_QSV_CAP_H264_BPYRAMID))
                    {
                        // with B-pyramid, the delay may be more than 1 frame,
                        // so compute the actual delay based on the initial DTS
                        // provided by MSDK; also, account for rounding errors
                        // (e.g. 24000/1001 fps @ 90kHz -> 3753.75 ticks/frame)
                        pv->bfrm_delay = ((task->bs->TimeStamp -
                                           task->bs->DecodeTimeStamp +
                                           (duration / 2)) / duration);
                        pv->bfrm_delay = FFMAX(pv->bfrm_delay, 1);
                        pv->bfrm_delay = FFMIN(pv->bfrm_delay, BFRM_DELAY_MAX);
                    }
                    /*
                     * Generate VFR-compatible output DTS based on input PTS.
                     *
                     * Depends on the B-frame delay:
                     *
                     * 0: ipts0,  ipts1, ipts2...
                     * 1: ipts0 - ipts1, ipts1 - ipts1, ipts1,  ipts2...
                     * 2: ipts0 - ipts2, ipts1 - ipts2, ipts2 - ipts2, ipts1...
                     * ...and so on.
                     */
                    if (pv->frames_out <= pv->bfrm_delay)
                    {
                        buf->s.renderOffset = (pv->init_pts[pv->frames_out] -
                                               pv->init_pts[pv->bfrm_delay]);
                    }
                    else
                    {
                        buf->s.renderOffset = hb_qsv_pop_next_dts(pv->list_dts);
                    }
                }

                /*
                 * In the MP4 container, DT(0) = STTS(0) = 0.
                 *
                 * Which gives us:
                 * CT(0) = CTTS(0) + STTS(0) = CTTS(0) = PTS(0) - DTS(0)
                 * When DTS(0) < PTS(0), we then have:
                 * CT(0) > 0 for video, but not audio (breaks A/V sync).
                 *
                 * This is typically solved by writing an edit list shifting
                 * video samples by the initial delay, PTS(0) - DTS(0).
                 *
                 * See:
                 * ISO/IEC 14496-12:2008(E), ISO base media file format
                 *  - 8.6.1.2 Decoding Time to Sample Box
                 */
                if (w->config->h264.init_delay == 0 && buf->s.renderOffset < 0)
                {
                    w->config->h264.init_delay = -buf->s.renderOffset;
                }
            }

            /*
             * If we have a chapter marker pending and this frame's
             * presentation time stamp is at or after the marker's time stamp,
             * use this as the chapter start.
             */
            if (pv->next_chapter.index && buf->s.frametype == HB_FRAME_IDR &&
                pv->next_chapter.start <= buf->s.start)
            {
                buf->s.new_chap = pv->next_chapter.index;
                pv->next_chapter.index = 0;
            }

                // shift for fifo
                if(pv->async_depth){
                    av_qsv_list_rem(qsv_encode->tasks,task);
                    av_qsv_list_add(qsv_encode->tasks,task);
                }

                task->bs->DataLength    = 0;
                task->bs->DataOffset    = 0;
                task->bs->MaxLength = qsv_encode->p_buf_max_size;
                task->stage        = 0;
                pv->frames_out++;
            }
        }
    }while(is_end);


    if(is_end){
        if( !buf && MFX_ERR_MORE_DATA == sts )
            break;

    }
    else
        break;

    }

    if(!is_end)
        ++pv->frames_in;

    if(is_end){
        *buf_in = NULL;
        if(last_buf){
            last_buf->next = in;
        }
        else
            *buf_out = in;
        return HB_WORK_DONE;
    }
    else{
        return HB_WORK_OK;
    }
}

int nal_find_start_code(uint8_t** pb, size_t* size){
    if ((int) *size < 4 )
        return 0;

    // find start code by MSDK , see ff_prefix_code[]
    while ((4 <= *size) &&
        ((0 != (*pb)[0]) ||
         (0 != (*pb)[1]) ||
         (1 != (*pb)[2]) ))
    {
        *pb += 1;
        *size -= 1;
    }

    if (4 <= *size)
        return (((*pb)[0] << 24) | ((*pb)[1] << 16) | ((*pb)[2] << 8) | ((*pb)[3]));

    return 0;
}

void parse_nalus(uint8_t *nal_inits, size_t length, hb_buffer_t *buf, uint32_t frame_num){
    uint8_t *offset = nal_inits;
    size_t size     = length;

    if( nal_find_start_code(&offset,&size) == 0 )
        size = 0;

    while( size > 0 ){

            uint8_t* current_nal = offset + sizeof(ff_prefix_code)-1;
            uint8_t *next_offset = offset + sizeof(ff_prefix_code);
            size_t next_size     = size - sizeof(ff_prefix_code);
            size_t current_size  = next_size;
            if( nal_find_start_code(&next_offset,&next_size) == 0 ){
                size = 0;
                current_size += 1;
            }
            else{
                current_size -= next_size;
                if( next_offset > 0 && *(next_offset-1) != 0  )
                    current_size += 1;
            }
            {
                char size_position[4] = {0,0,0,0};
                size_position[1] = (current_size >> 24) & 0xFF;
                size_position[1] = (current_size >> 16) & 0xFF;
                size_position[2] = (current_size >> 8)  & 0xFF;
                size_position[3] =  current_size        & 0xFF;

                memcpy(buf->data + buf->size,&size_position ,sizeof(size_position));
                buf->size += sizeof(size_position);

                memcpy(buf->data + buf->size,current_nal ,current_size);
                buf->size += current_size;
            }

            if(size){
                size   = next_size;
                offset = next_offset;
            }
        }
}

int hb_qsv_h264_param_init_for_job(hb_qsv_param_t *param, hb_job_t *job)
{
    if (param != NULL && job != NULL)
    {
        /* enable and set colorimetry (video signal information) */
        param->videoSignalInfo.ColourDescriptionPresent = 1;
        switch (job->color_matrix_code)
        {
            case 4:
                // custom
                param->videoSignalInfo.ColourPrimaries         = job->color_prim;
                param->videoSignalInfo.TransferCharacteristics = job->color_transfer;
                param->videoSignalInfo.MatrixCoefficients      = job->color_matrix;
                break;
            case 3:
                // ITU BT.709 HD content
                param->videoSignalInfo.ColourPrimaries         = HB_COLR_PRI_BT709;
                param->videoSignalInfo.TransferCharacteristics = HB_COLR_TRA_BT709;
                param->videoSignalInfo.MatrixCoefficients      = HB_COLR_MAT_BT709;
                break;
            case 2:
                // ITU BT.601 DVD or SD TV content (PAL)
                param->videoSignalInfo.ColourPrimaries         = HB_COLR_PRI_EBUTECH;
                param->videoSignalInfo.TransferCharacteristics = HB_COLR_TRA_BT709;
                param->videoSignalInfo.MatrixCoefficients      = HB_COLR_MAT_SMPTE170M;
                break;
            case 1:
                // ITU BT.601 DVD or SD TV content (NTSC)
                param->videoSignalInfo.ColourPrimaries         = HB_COLR_PRI_SMPTEC;
                param->videoSignalInfo.TransferCharacteristics = HB_COLR_TRA_BT709;
                param->videoSignalInfo.MatrixCoefficients      = HB_COLR_MAT_SMPTE170M;
                break;
            default:
                // detected during scan
                param->videoSignalInfo.ColourPrimaries         = job->title->color_prim;
                param->videoSignalInfo.TransferCharacteristics = job->title->color_transfer;
                param->videoSignalInfo.MatrixCoefficients      = job->title->color_matrix;
                break;
        }

        /* parse user-specified advanced options */
        hb_qsv_param_parse_all(param, job->advanced_opts, job->vcodec);

        /* reload colorimetry in case values were set in advanced_opts */
        job->color_matrix_code = 4;
        job->color_prim        = param->videoSignalInfo.ColourPrimaries;
        job->color_transfer    = param->videoSignalInfo.TransferCharacteristics;
        job->color_matrix      = param->videoSignalInfo.MatrixCoefficients;

        /* encode to H.264 and set FrameInfo */
        param->videoParam.mfx.CodecId                 = MFX_CODEC_AVC;
        param->videoParam.mfx.CodecLevel              = MFX_LEVEL_UNKNOWN;
        param->videoParam.mfx.CodecProfile            = MFX_PROFILE_UNKNOWN;
        param->videoParam.mfx.FrameInfo.FourCC        = MFX_FOURCC_NV12;
        param->videoParam.mfx.FrameInfo.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
        param->videoParam.mfx.FrameInfo.CropX         = 0;
        param->videoParam.mfx.FrameInfo.CropY         = 0;
        param->videoParam.mfx.FrameInfo.CropW         = job->width;
        param->videoParam.mfx.FrameInfo.CropH         = job->height;
        param->videoParam.mfx.FrameInfo.AspectRatioW  = job->anamorphic.par_width;
        param->videoParam.mfx.FrameInfo.AspectRatioH  = job->anamorphic.par_height;
        param->videoParam.mfx.FrameInfo.Width         = AV_QSV_ALIGN16(job->width);
        param->videoParam.mfx.FrameInfo.Height        = AV_QSV_ALIGN16(job->height);
        if (param->videoParam.mfx.FrameInfo.PicStruct != MFX_PICSTRUCT_PROGRESSIVE)
        {
            param->videoParam.mfx.FrameInfo.Height = AV_QSV_ALIGN32(job->height);
        }
        hb_limit_rational64((int64_t*)&param->videoParam.mfx.FrameInfo.FrameRateExtN,
                            (int64_t*)&param->videoParam.mfx.FrameInfo.FrameRateExtD,
                            job->vrate, job->vrate_base, UINT32_MAX);

        /* set H.264 profile and level */
        if (job->h264_profile != NULL && job->h264_profile[0] != '\0' &&
            strcasecmp(job->h264_profile, "auto"))
        {
            if (!strcasecmp(job->h264_profile, "baseline"))
            {
                param->videoParam.mfx.CodecProfile = MFX_PROFILE_AVC_BASELINE;
            }
            else if (!strcasecmp(job->h264_profile, "main"))
            {
                param->videoParam.mfx.CodecProfile = MFX_PROFILE_AVC_MAIN;
            }
            else if (!strcasecmp(job->h264_profile, "high"))
            {
                param->videoParam.mfx.CodecProfile = MFX_PROFILE_AVC_HIGH;
            }
            else
            {
                hb_log("hb_qsv_h264_param_init_for_job: bad profile %s",
                       job->h264_profile);
                return -1;
            }
        }
        if (job->h264_level != NULL && job->h264_level[0] != '\0' &&
            strcasecmp(job->h264_level, "auto"))
        {
            int err;
            int i = hb_qsv_atoindex(hb_h264_level_names, job->h264_level, &err);
            if (err || i >= (sizeof(hb_h264_level_values) /
                             sizeof(hb_h264_level_values[0])))
            {
                hb_log("hb_qsv_h264_param_init_for_job: bad level %s",
                       job->h264_level);
                return -1;
            }
            else if (hb_qsv_info->capabilities & HB_QSV_CAP_MSDK_API_1_6)
            {
                param->videoParam.mfx.CodecLevel = HB_QSV_CLIP3(MFX_LEVEL_AVC_1,
                                                                MFX_LEVEL_AVC_52,
                                                                hb_h264_level_values[i]);
            }
            else
            {
                // Media SDK API < 1.6, MFX_LEVEL_AVC_52 unsupported
                param->videoParam.mfx.CodecLevel = HB_QSV_CLIP3(MFX_LEVEL_AVC_1,
                                                                MFX_LEVEL_AVC_51,
                                                                hb_h264_level_values[i]);
            }
        }

        /* set rate control paremeters */
        if (job->vquality >= 0)
        {
            // introduced in API 1.1
            param->videoParam.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
            param->videoParam.mfx.QPI = HB_QSV_CLIP3(0, 51, job->vquality + param->rc.cqp_offsets[0]);
            param->videoParam.mfx.QPP = HB_QSV_CLIP3(0, 51, job->vquality + param->rc.cqp_offsets[1]);
            param->videoParam.mfx.QPB = HB_QSV_CLIP3(0, 51, job->vquality + param->rc.cqp_offsets[2]);
        }
        else if (job->vbitrate > 0)
        {
            if (hb_qsv_info->capabilities & HB_QSV_CAP_OPTION2_LOOKAHEAD)
            {
                if (param->rc.lookahead < 0)
                {
                    if (param->rc.vbv_max_bitrate > 0)
                    {
                        // lookahead RC doesn't support VBV
                        param->rc.lookahead = 0;
                    }
                    else
                    {
                        // set automatically based on target usage
                        param->rc.lookahead = (param->videoParam.mfx.TargetUsage >= MFX_TARGETUSAGE_2);
                    }
                }
                else
                {
                    // user force-enabled or force-disabled lookahead RC
                    param->rc.lookahead = !!param->rc.lookahead;
                }
            }
            else
            {
                // lookahead RC not supported
                param->rc.lookahead = 0;
            }
            if (param->rc.lookahead)
            {
                // introduced in API 1.7
                param->videoParam.mfx.RateControlMethod = MFX_RATECONTROL_LA;
                param->videoParam.mfx.TargetKbps        = job->vbitrate;
                if (param->rc.vbv_max_bitrate > 0)
                {
                    hb_log("hb_qsv_h264_param_init_for_job: MFX_RATECONTROL_LA, ignoring VBV");
                }
            }
            else if (job->vbitrate == param->rc.vbv_max_bitrate)
            {
                // introduced in API 1.0
                param->videoParam.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
                param->videoParam.mfx.MaxKbps           = job->vbitrate;
                param->videoParam.mfx.TargetKbps        = job->vbitrate;
                param->videoParam.mfx.BufferSizeInKB    = (param->rc.vbv_buffer_size / 8);
                if (param->rc.vbv_buffer_size <= 0)
                {
                    // let Media SDK calculate these for us
                    param->videoParam.mfx.BufferSizeInKB   = 0;
                    param->videoParam.mfx.InitialDelayInKB = 0;
                }
                else if (param->rc.vbv_buffer_init > 1.0)
                {
                    param->videoParam.mfx.InitialDelayInKB = (param->rc.vbv_buffer_init / 8);
                }
                else
                {
                    param->videoParam.mfx.InitialDelayInKB = (param->rc.vbv_buffer_size *
                                                              param->rc.vbv_buffer_init / 8);
                }
            }
            else if (param->rc.vbv_max_bitrate > 0)
            {
                // introduced in API 1.0
                param->videoParam.mfx.RateControlMethod = MFX_RATECONTROL_VBR;
                param->videoParam.mfx.MaxKbps           = param->rc.vbv_max_bitrate;
                param->videoParam.mfx.TargetKbps        = job->vbitrate;
                param->videoParam.mfx.BufferSizeInKB    = (param->rc.vbv_buffer_size / 8);
                if (param->rc.vbv_buffer_size <= 0)
                {
                    // let Media SDK calculate these for us
                    param->videoParam.mfx.BufferSizeInKB   = 0;
                    param->videoParam.mfx.InitialDelayInKB = 0;
                }
                else if (param->rc.vbv_buffer_init > 1.0)
                {
                    param->videoParam.mfx.InitialDelayInKB = (param->rc.vbv_buffer_init / 8);
                }
                else
                {
                    param->videoParam.mfx.InitialDelayInKB = (param->rc.vbv_buffer_size *
                                                              param->rc.vbv_buffer_init / 8);
                }
            }
            else
            {
                // introduced in API 1.3
                param->videoParam.mfx.RateControlMethod = MFX_RATECONTROL_AVBR;
                param->videoParam.mfx.TargetKbps        = job->vbitrate;
                param->videoParam.mfx.Convergence       = 10; // 1000 frames
                param->videoParam.mfx.Accuracy          = 50; // +/- 5.0%
            }
        }
        else
        {
            hb_error("hb_qsv_h264_param_init_for_job: invalid rate control (%d, %d)",
                     job->vquality, job->vbitrate);
            return -1;
        }

        /* set the keyframe interval */
        if (param->gop.gop_pic_size < 0)
        {
            int rate = (int)((double)job->vrate / (double)job->vrate_base + 0.5);
            if (param->videoParam.mfx.RateControlMethod == MFX_RATECONTROL_CQP)
            {
                // ensure B-pyramid is enabled for CQP on Haswell
                param->gop.gop_pic_size = 32;
            }
            else
            {
                // set the keyframe interval based on the framerate
                param->gop.gop_pic_size = 5 * rate + 1;
            }
        }
        param->videoParam.mfx.GopPicSize = param->gop.gop_pic_size;
    }
    else
    {
        hb_error("hb_qsv_h264_param_init_for_job: NULL pointer");
        return -1;
    }
    return 0;
}

mfxStatus hb_qsv_h264_param_get_esconfig(hb_qsv_param_t *param,
                                         hb_esconfig_t *config)
{
    mfxStatus err = MFX_ERR_NONE;
    if (param != NULL && config != NULL)
    {
        mfxStatus err;
        mfxVersion version;
        mfxVideoParam videoParam;
        mfxSession session = (mfxSession)0;
        mfxExtCodingOptionSPSPPS sps_pps_buf, *sps_pps = &sps_pps_buf;
        version.Major = HB_QSV_MINVERSION_MAJOR;
        version.Minor = HB_QSV_MINVERSION_MINOR;
        err = MFXInit(MFX_IMPL_AUTO_ANY, &version, &session);
        if (err != MFX_ERR_NONE)
        {
            return err;
        }
        err = MFXVideoENCODE_Init(session, &param->videoParam);
        if (err != MFX_ERR_NONE &&
            err != MFX_WRN_PARTIAL_ACCELERATION &&
            err != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM)
        {
            MFXClose(session);
            return err;
        }
        /* Get the SPS/PPS */
        memset(&videoParam, 0, sizeof(mfxVideoParam));
        sps_pps->Header.BufferId = MFX_EXTBUFF_CODING_OPTION_SPSPPS;
        sps_pps->Header.BufferSz = sizeof(mfxExtCodingOptionSPSPPS);
        sps_pps->SPSId           = 0;
        sps_pps->SPSBuffer       = config->h264.sps;
        sps_pps->SPSBufSize      = sizeof(config->h264.sps);
        sps_pps->PPSId           = 0;
        sps_pps->PPSBuffer       = config->h264.pps;
        sps_pps->PPSBufSize      = sizeof(config->h264.pps);
        videoParam.NumExtParam   = 1;
        videoParam.ExtParam      = (mfxExtBuffer**)&sps_pps;
        err = MFXVideoENCODE_GetVideoParam(session, &videoParam);
        MFXVideoENCODE_Close(session);
        MFXClose(session);
        if (err == MFX_ERR_NONE)
        {
            // remove 32-bit NAL prefix (0x00 0x00 0x00 0x01)
            config->h264.sps_length = sps_pps->SPSBufSize - 4;
            memmove(config->h264.sps, config->h264.sps + 4, config->h264.sps_length);
            config->h264.pps_length = sps_pps->PPSBufSize - 4;
            memmove(config->h264.pps, config->h264.pps + 4, config->h264.pps_length);
        }
        else
        {
            config->h264.sps_length = config->h264.pps_length = 0;
        }
    }
    else
    {
        hb_error("hb_qsv_h264_param_init_for_job: NULL pointer");
        err = MFX_ERR_NULL_PTR;
        goto end;
    }
end:
    return err;
}
