#include "stdafx.h"  

#ifdef __cplusplus  
extern "C"
{
#endif  

#include<tchar.h>  

#include <stdio.h>  
#include <stdlib.h>  
#include <string.h>  
#include"common.h"  

#include"mux.h"  
#include"logger.h"  

#define CLEAN_AND_EXIT {MuxCallbackFunction((AVMuxCallbackFunction)avm->vp->notification, avm->vp, -1, /*avm->vp->progress*/0);\  
	CtxAllFree(avm); \
		return -1; }

#ifdef __cplusplus  
};
#endif  

/*
* status, -1 for failed, 0 for encode succeed, 1 for setting stop, 2 for calback progress, 3 for mux succeed
*/

/*
* 释放结构体内存
*/
static void CtxAllFree(AVMuxing *avm);

/*
* 计算进度
*/
static int CurrentPercent(int cnt, int total);

static void MuxCallbackFunction(AVMuxCallbackFunction fcn, void *identifier, int status, int percent)
{
	if (fcn != NULL) {
		fcn(identifier, status, percent);
	}
	else {
		//LogPrint();  
	}
}

/*
* 判断输入音频是否有效
*/
static int OpenInputFile(const char *filename)
{
	FILE *fp;
	fp = fopen(filename, "rb");
	if (fp == NULL) {
		LOG_PRINT("%s....Audio fopen failed", __FUNCTION__);
		return -1;
	}
	/* fp如果为空，feek会出错，根本走不到判断audio_len那里 */
	fseek(fp, 0L, SEEK_END);
	int len = ftell(fp);
	/* 即时是局部变量，也调用fclose */
	fclose(fp);
	return len;
}

/* 将临时文件拷贝到目的路径，用在没有音频的时候 */
void* copy_file_thread(void *arg)
{
	__try {
		int ret;
		LOG_PRINT("%s...1", __FUNCTION__);
		AVMuxing *avm = (AVMuxing*)arg;
		LOG_PRINT("%s....tmp_path: %s, path: %s", __FUNCTION__, avm->vp->tmp_path, avm->vp->path);
		bool flag = CopyFileA(avm->vp->tmp_path, avm->vp->path, FALSE);
		if (!flag) {
			TCHAR szBuf[128];
			LPVOID lpMsgBuf;
			DWORD dw = GetLastError();
			FormatMessage(
				FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
				NULL,
				dw,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				(LPTSTR)&lpMsgBuf,
				0, NULL);
			wsprintf(szBuf,
				_T("%s error message (error code = %d): %s"),
				_T("CopyFileA"), dw, lpMsgBuf);
			LocalFree(lpMsgBuf);
			LOG_PRINT("%s...%s", __FUNCTION__, szBuf);
			MuxCallbackFunction((AVMuxCallbackFunction)avm->vp->notification, avm->vp, -1, avm->vp->progress);
			CtxAllFree(avm);
			return NULL;
		}

		LOG_PRINT("%s....======copy_file_thread end ======!", __FUNCTION__);
		MuxCallbackFunction((AVMuxCallbackFunction)avm->vp->notification, avm->vp, 3, 100);
		CtxAllFree(avm);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LOG_PRINT("%s error\n", __FUNCTION__);
	}
	return NULL;
}

/*
* 将音频和视频复用，判断音频包和视频包的pts，pts在前的先写入输出文件
* 当音频包没了，继续写入视频包
*/
void* mux_thread(void* avmuxing)
{
	__try {
		LOG_PRINT("%s...1", __FUNCTION__);
		AVMuxing *avm = (AVMuxing*)avmuxing;
		AVPacket pkt;
		int frame_index = 0;
		int64_t cur_pts_v = 0, cur_pts_a = 0;
		bool flag = FALSE;
		int compare_tag = -1;
		bool audio_pkt_done = FALSE;    //音频包写完标志，即使音频包写完了还要继续写视频包  
		int pkt_cnt = 1;
		int i_v_nb_frames = avm->vp->fmt_ctx->streams[0]->nb_frames;

		while (1) {
			pthread_mutex_lock(&(avm->vp->pp->mutex_t));
			if (avm->vp->is_stop) {
				flag = TRUE;
			}
			pthread_mutex_unlock(&(avm->vp->pp->mutex_t));
			if (flag) {
				break;
			}
			int stream_index = 0;
			AVFormatContext *i_fmt_ctx;
			AVStream *in_stream, *out_stream;

			/* 比较音频包和视频包的pts */
			if (!audio_pkt_done) {
				if (avm->audio_len > 0) {
					compare_tag = av_compare_ts(cur_pts_v, avm->i_fmt_ctx_v->streams[avm->videoindex_v]->time_base,
						cur_pts_a, avm->i_fmt_ctx_a->streams[avm->audioindex_a]->time_base);
				}
			}
			if (compare_tag <= 0) {      /* 视频包在前，写入视频包 */
				i_fmt_ctx = avm->i_fmt_ctx_v;
				stream_index = avm->videoindex_out;

				if (av_read_frame(i_fmt_ctx, &pkt) >= 0) {
					do {
						in_stream = i_fmt_ctx->streams[pkt.stream_index];
						out_stream = avm->o_fmt_ctx->streams[stream_index];
						//out_stream->time_base = { 1, vp->fps };     //force out_stream to AVCodecContext->fps  
						if (pkt.stream_index == avm->videoindex_v) {
							if (pkt.pts == AV_NOPTS_VALUE) {
								AVRational time_base1 = in_stream->time_base;
								int64_t calc_duration = (double)AV_TIME_BASE / av_q2d(in_stream->r_frame_rate);
								pkt.pts = (double)(frame_index * calc_duration) / (double)(av_q2d(time_base1) * AV_TIME_BASE);
								pkt.dts = pkt.pts;
								pkt.duration = (double)calc_duration / (double)(av_q2d(time_base1) * AV_TIME_BASE);
								frame_index++;
							}
							cur_pts_v = pkt.pts;

							/* 在此回调进度 */
							avm->vp->progress = CurrentPercent(++pkt_cnt, i_v_nb_frames);
							MuxCallbackFunction((AVMuxCallbackFunction)avm->vp->notification, avm->vp, 2, avm->vp->progress);
							break;
						}

					} while (av_read_frame(i_fmt_ctx, &pkt) >= 0);
				}
				else {
					LOG_PRINT("%s....av_read_frame < 0", __FUNCTION__);
					break;
				}
			}
			else {      /* 音频包在前，写入音频包 */
				i_fmt_ctx = avm->i_fmt_ctx_a;
				stream_index = avm->audioindex_out;
				if (av_read_frame(i_fmt_ctx, &pkt) >= 0) {
					do {
						in_stream = i_fmt_ctx->streams[pkt.stream_index];
						out_stream = avm->o_fmt_ctx->streams[stream_index];
						if (pkt.stream_index == avm->audioindex_a) {
							//TODO:add audio filter here to format pkt!  
							if (pkt.pts == AV_NOPTS_VALUE) {
								AVRational time_base1 = in_stream->time_base;
								int64_t calc_duration = (double)AV_TIME_BASE / av_q2d(in_stream->r_frame_rate);
								pkt.pts = (double)(frame_index * calc_duration) / (double)(av_q2d(time_base1) * AV_TIME_BASE);
								pkt.dts = pkt.pts;
								pkt.duration = (double)calc_duration / (double)(av_q2d(time_base1) * AV_TIME_BASE);
								frame_index++;
							}
							cur_pts_a = pkt.pts;
							break;
						}
					} while (av_read_frame(i_fmt_ctx, &pkt) >= 0);
				}
				else {
					//break;    /* 如果音频比视频短，不break，以视频为标准 */  
					/* 音频包写完后，继续写完视频包 */
					audio_pkt_done = TRUE;
					compare_tag = -1;
					continue;
				}
			}

			//Convert PTS/DTS  
			pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
			pkt.pos = -1;
			pkt.stream_index = stream_index;
#ifdef DEBUG  
			printf("Write 1 Packet. Size:%5d\tpts:%5lld\tdts:%5lld\n", pkt.size, pkt.pts, pkt.dts);
#endif  
			//Write packet to file  
			if (av_interleaved_write_frame(avm->o_fmt_ctx, &pkt) < 0) {
				LOG_PRINT("%s....av_interleaved_write_frame failed", __FUNCTION__);
				MuxCallbackFunction((AVMuxCallbackFunction)avm->vp->notification, avm->vp, -1, avm->vp->progress);
				CtxAllFree(avm);
				return NULL;
				//break;  
			}
			av_free_packet(&pkt);
		}

		pthread_mutex_lock(&(avm->vp->pp->mutex_t));
		if (avm->vp->is_stop) {
			flag = TRUE;
		}
		pthread_mutex_unlock(&(avm->vp->pp->mutex_t));
		if (flag) {
			MuxCallbackFunction((AVMuxCallbackFunction)avm->vp->notification, avm->vp, 1, avm->vp->progress);
			CtxAllFree(avm);
			return NULL;
		}
		//Write file trailer  
		av_write_trailer(avm->o_fmt_ctx);

#ifdef DEBUG  
		printf("======muxer success ======!\n");
#endif  
		LOG_PRINT("%s....======mux_thread end ======!", __FUNCTION__);
		MuxCallbackFunction((AVMuxCallbackFunction)avm->vp->notification, avm->vp, 3, 100);
		CtxAllFree(avm);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LOG_PRINT("%s error\n", __FUNCTION__);
	}
	return NULL;
}

int MuxAudioVideo(videoParm *vp, const char *inAudio)
{
	LOG_PRINT("%s....width = %d, height = %d, fps = %d, format = %s, outpath = %s", __FUNCTION__,
		vp->width, vp->height, vp->fps, vp->format, vp->path);
	LOG_PRINT("%s....1", __FUNCTION__);

	AVMuxing *avm = (AVMuxing*)malloc(sizeof(AVMuxing));
	memset(avm, 0, sizeof(AVMuxing));

	avm->vp = vp;
	avm->i_fmt_ctx_a = NULL;
	avm->i_fmt_ctx_v = NULL;
	avm->o_fmt_ctx = NULL;
	avm->enc_ctx_v = NULL;
	avm->enc_ctx_a = NULL;
	avm->in_audio = inAudio;

	/*
	* 当外部没有音频（或生成音频失败）则创建拷贝文件线程，将临时文件拷贝到目的路径
	*/
	if (!strcmp("", inAudio)) {
		pthread_create(&avm->vp->pp->tid, NULL, copy_file_thread, avm);
		/* 线程退出时自动释放资源 */
		pthread_detach(avm->vp->pp->tid);
		LOG_PRINT("%s....Create copy file thread", __FUNCTION__);
		return 0;
	}

	AVOutputFormat *o_fmt;

	int ret = -1, i = -1;
	avm->videoindex_v = -1;
	avm->videoindex_out = -1;
	avm->audioindex_a = -1;
	avm->audioindex_out = -1;
	bool flag = FALSE;
	const char *in_video = avm->vp->tmp_path;
	const char *in_audio = avm->in_audio;
	const char *out_file = avm->vp->path;

#ifdef DEBUG  
	printf("=========input video: %s\n", in_video);
	printf("=========input audio: %s\n", in_audio);
#endif  
	if ((avm->audio_len = OpenInputFile(in_audio)) == -1) {
		LOG_PRINT("%s....OpenInputFile failed", __FUNCTION__, ret);
		CLEAN_AND_EXIT
	}
	/* 注册所有编解码器和协议 */
	av_register_all();
	/* 打开输入音频文件， 返回AVFormatContext */
	if ((ret = avformat_open_input(&avm->i_fmt_ctx_a, in_audio, NULL, NULL)) < 0) {
		LOG_PRINT("%s....audio avformat_open_input failed,error code is: %d", __FUNCTION__, ret);
		CLEAN_AND_EXIT
	}
	/* 查找音频流信息 */
	if ((ret = avformat_find_stream_info(avm->i_fmt_ctx_a, NULL)) < 0) {
		LOG_PRINT("%s....audio avformat_find_stream_info failed,error code is: %d", __FUNCTION__, ret);
		if (avm->audio_len > 0) {
			LOG_PRINT("%s....Audio is Null", __FUNCTION__);
			CLEAN_AND_EXIT
		}
	}
	if ((ret = avformat_open_input(&avm->i_fmt_ctx_v, in_video, NULL, NULL)) < 0) {
		LOG_PRINT("%s....video avformat_open_input failed,error code is: %d", __FUNCTION__, ret);
		CLEAN_AND_EXIT
	}
	if ((ret = avformat_find_stream_info(avm->i_fmt_ctx_v, NULL)) < 0) {
		LOG_PRINT("%s....video avformat_find_stream_info failed,error code is: %d", __FUNCTION__, ret);
		CLEAN_AND_EXIT
	}
#ifdef DEBUG  
	printf("========================Input Infomation==================================\n");
	av_dump_format(avm->i_fmt_ctx_v, 0, in_video, 0);
	av_dump_format(avm->i_fmt_ctx_a, 0, in_audio, 0);
	printf("==========================================================================\n");
#endif  
	LOG_PRINT("%s....width = %d, height = %d, fps = %d, format = %s, outpath = %s", __FUNCTION__,
		avm->vp->width, avm->vp->height, avm->vp->fps, avm->vp->format, avm->vp->path);
	ret = avformat_alloc_output_context2(&avm->o_fmt_ctx, NULL, NULL, out_file);
	if (ret < 0) {
		LOG_PRINT("%s....avformat_alloc_output_context2 failed, error code: %d, out file is: %s", __FUNCTION__, ret, out_file);
		CLEAN_AND_EXIT
	}
	o_fmt = avm->o_fmt_ctx->oformat;

	for (i = 0; i < avm->i_fmt_ctx_v->nb_streams; i++) {
		pthread_mutex_lock(&(avm->vp->pp->mutex_t));
		if (avm->vp->is_stop) {
			flag = TRUE;
		}
		pthread_mutex_unlock(&(avm->vp->pp->mutex_t));
		if (flag) {
			break;
		}
		if (avm->i_fmt_ctx_v->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			AVStream *in_stream = avm->i_fmt_ctx_v->streams[i];
			avm->enc_ctx_v = avcodec_alloc_context3(NULL);
			if ((ret = avcodec_parameters_to_context(avm->enc_ctx_v, in_stream->codecpar)) < 0) {
				LOG_PRINT("%s....Video avcodec_parameters_to_context failed,error code is: %d", __FUNCTION__, ret);
				CLEAN_AND_EXIT
			}
			AVStream *out_stream = avformat_new_stream(avm->o_fmt_ctx, avm->enc_ctx_v->codec);
			if (!out_stream) {
				LOG_PRINT("%s....Video avformat_new_stream failed", __FUNCTION__);
				CLEAN_AND_EXIT
			}
			//out_stream->time_base = { 1, avm->vp->fps };     //force out_stream to AVCodecContext->fps  
			out_stream->time_base.num = 1;
			out_stream->time_base.den = avm->vp->fps;
			avm->videoindex_v = i;
			avm->videoindex_out = out_stream->index;

			avm->enc_ctx_v->codec_tag = 0;
			if (o_fmt->flags & AVFMT_GLOBALHEADER) {
				avm->enc_ctx_v->flags |= CODEC_FLAG_GLOBAL_HEADER;
			}
			if ((ret = avcodec_parameters_from_context(out_stream->codecpar, avm->enc_ctx_v)) < 0) {
				LOG_PRINT("%s....Video avcodec_parameters_from_context,error code is: %d", __FUNCTION__, ret);
				//CLEAN_AND_EXIT  
			}
			break;
		}
	}

	if (avm->audio_len > 0) {
		for (i = 0; i < avm->i_fmt_ctx_a->nb_streams; i++) {
			pthread_mutex_lock(&(avm->vp->pp->mutex_t));
			if (avm->vp->is_stop) {
				flag = TRUE;
			}
			pthread_mutex_unlock(&(avm->vp->pp->mutex_t));
			if (flag) {
				break;
			}
#ifdef DEBUG  
			printf("===========streams number:%3d===========\n", avm->i_fmt_ctx_a->nb_streams);
#endif  
			if (avm->i_fmt_ctx_a->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				AVStream *in_stream = avm->i_fmt_ctx_a->streams[i];
				avm->enc_ctx_a = avcodec_alloc_context3(NULL);
				if ((ret = avcodec_parameters_to_context(avm->enc_ctx_a, in_stream->codecpar)) < 0) {
					LOG_PRINT("%s....Audio avcodec_parameters_to_context failed,error code is: %d", __FUNCTION__, ret);
					CLEAN_AND_EXIT
				}
				AVStream *out_stream = avformat_new_stream(avm->o_fmt_ctx, avm->enc_ctx_a->codec);
				if (!out_stream) {
					LOG_PRINT("%s....Audio avformat_new_stream failed", __FUNCTION__);
					CLEAN_AND_EXIT
				}
				avm->audioindex_a = i;
				avm->audioindex_out = out_stream->index;

				out_stream->codecpar->codec_tag = 0;
				if (o_fmt->flags & AVFMT_GLOBALHEADER) {
					avm->enc_ctx_a->flags |= CODEC_FLAG_GLOBAL_HEADER;
				}
				if ((ret = avcodec_parameters_from_context(out_stream->codecpar, avm->enc_ctx_a)) < 0) {
					LOG_PRINT("%s....Audio avcodec_parameters_from_context,error code is: %d", __FUNCTION__, ret);
					CLEAN_AND_EXIT
				}
				break;
			}
		}
	}
#ifdef DEBUG  
	printf("========Output Infomation=======\n");
	av_dump_format(avm->o_fmt_ctx, 0, out_file, 1);
	printf("================================\n");
#endif  

	//Open output file  
	if (!(o_fmt->flags & AVFMT_NOFILE)) {
		if ((ret = avio_open(&avm->o_fmt_ctx->pb, out_file, AVIO_FLAG_WRITE)) < 0) {
			LOG_PRINT("%s....avio_open failed,error code is: %d", __FUNCTION__, ret);
			CLEAN_AND_EXIT
		}
	}

	if (avm->o_fmt_ctx->pb == NULL)
		LOG_PRINT("%s....avm->o_fmt_ctx->pb == NULL", __FUNCTION__);
	if (avm->o_fmt_ctx->oformat == NULL)
		LOG_PRINT("%s....avm->o_fmt_ctx->oformat == NULL", __FUNCTION__);

	//Write file header  
	int header_ret = avformat_write_header(avm->o_fmt_ctx, NULL);
	if (header_ret < 0) {
		if (avm->o_fmt_ctx == NULL) {
			LOG_PRINT("%s....avm->o_fmt_ctx is NULL", __FUNCTION__);
		}
		LOG_PRINT("%s....avformat_write_header failed,error code is: %d", __FUNCTION__, header_ret);
		CLEAN_AND_EXIT
	}

	/*
	* 创建复用线程
	*/
	pthread_create(&avm->vp->pp->tid, NULL, mux_thread, avm);
	/* 线程退出时自动释放资源 */
	pthread_detach(avm->vp->pp->tid);
	LOG_PRINT("%s....Create mux thread", __FUNCTION__);
	return 0;
}

static void CtxAllFree(AVMuxing *avm)
{
	__try {
		if (avm->enc_ctx_a) {
			avcodec_close(avm->enc_ctx_a);
		}
		if (avm->enc_ctx_v) {
			avcodec_close(avm->enc_ctx_v);
		}
		if (avm->i_fmt_ctx_a) {
			avformat_close_input(&avm->i_fmt_ctx_a);
		}
		if (avm->i_fmt_ctx_v) {
			avformat_close_input(&avm->i_fmt_ctx_v);
		}
		if (avm->o_fmt_ctx) {
			avio_close(avm->o_fmt_ctx->pb);
			avformat_free_context(avm->o_fmt_ctx);
		}
		if (avm) {
			free(avm);
		}
		/* 设为NULL，避免使用野指针 */
		avm = NULL;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LOG_PRINT("%s error\n", __FUNCTION__);
	}
}

int CurrentPercent(int cnt, int total)
{
	int ret = -1;
	ret = (int)((float)cnt / (float)total * 100.0);
	printf("Progress is %d\n", ret);

	return ret;
}