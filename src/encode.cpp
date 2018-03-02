#include "stdafx.h"  

//#define DEBUG_   

#ifdef DEBUG_  
#include<timeapi.h>  
#pragma comment(lib, "winmm.lib ")  
#endif  

#define REMOVE_TEMP_FILE  

#ifdef __cplusplus  
extern "C"
{
#endif  
#include <stdio.h>  
#include <stdlib.h>  
#include <string.h>  
#include <sys/stat.h>  
#include <errno.h>  

#include"common.h"  

#include <io.h>  
#include<direct.h>  

#include"encode.h"  
#include"logger.h"  

#ifdef __cplusplus  
};
#endif  

#define ERR_NULL_EXIT {callback_function((EncodeCallbackFunction)vp->notification, vp, -1, 0);\  
return NULL; \
}

static void* encode_thread(void *videoparm);

/*
* 将编码器中剩余的几帧写入文件
*/
static void flush_encode(videoParm *vp);

/*
* end_push后将fifo_buf中剩余的帧写入文件
*/
static int encode_remaining(videoParm *vp);

/*
* 填充codec_ctx, 和dict，平衡比特率，编码速度，画质
*/
static void codecCtx_fill(AVCodecContext *codec_ctx, AVDictionary **dict, const char *out_fmt);

/*
* 分配argb帧（yuv帧）内存，先让data指针和linesize指针指向buf，后面再从文件中写入数据到buf
*/
static int frame_buf_alloc(videoParm *vp, int width, int height);

/*
* 该源文件中返回的百分比都是0，因为encode的时候的进度由外部计算，到add_audio的时候要直接算
*/
static void callback_function(EncodeCallbackFunction fun, void *identifier, int status, int percent);

static void encode_delay_time(pthread_mutex_t *p_mutex, pthread_cond_t *p_cond, uint64_t ms);

/*
* 编码时创建的临时文件，用以在mux.cpp中与传入的音频合成
*/
static char* EncodeCreateTempFile(const char *ext);

static char* encode_parm_copy(const char *src);

/* ANSI */
static char *EncodeCreateTempDir(char *sub_dir);

#ifdef DEBUG_  
/* 监视push帧的次数 */
int g_push_cnt = 0;
/* 监视实际写入帧的次数 */
int g_write_frame_cnt = 0;
#endif  

/* 由于线程是异步的，当外部调用preparing_to_push函数后，外部传进来的指针变量如format或path，实际内存已经释放
* 所以需要在prepare函数动态分配内存保存指针变量
* 这些动态分配内存的变量最终在release函数中释放内存
*/
static char* encode_parm_copy(const char *src)
{
	char *dst;
	size_t len = strlen(src) + 1;
	dst = (char *)malloc(len);
	if (dst == NULL) {
		LOG_PRINT("%s....Malloc failed", __FUNCTION__);
		return NULL;
	}
	memset(dst, 0, len);
	memcpy(dst, src, strlen(src));
	return dst;
}

void *preparing_to_push(int width, int height, int fps, int audio_exist, const char *format, const char *path)
{

	__try {
		LOG_PRINT("%s....1", __FUNCTION__);
#ifdef DEBUG_  
		g_push_cnt = 0;
		g_write_frame_cnt = 0;
#endif  
		videoParm *vp = (videoParm*)av_malloc(sizeof(videoParm));
		if (vp == NULL)
		{
			LOG_PRINT("%s....videoParm allocation failed", __FUNCTION__);
			ERR_NULL_EXIT
		}
		memset(vp, 0, sizeof(videoParm));

		vp->fmt_ctx = NULL;
		vp->codec_ctx = NULL;
		vp->stream = NULL;
		vp->sws_ctx = NULL;
		vp->width = width;
		vp->height = height;
		vp->fps = fps;
		vp->audio_exist = audio_exist;
		//vp->audio_exist = TRUE;  
		/* Malloc内存保存char*变量foramt,path，防止主线程结束，子线程变量丢失内容 */
		vp->format = encode_parm_copy(format);
		vp->path = encode_parm_copy(path);
		if (vp->format == NULL || vp->path == NULL) {
			LOG_PRINT("%s....encode_parm_copy failed", __FUNCTION__);
			ERR_NULL_EXIT
		}
		vp->pp = (pthread_Parm*)malloc(sizeof(pthread_Parm));
		memset(vp->pp, 0, sizeof(pthread_Parm));
		vp->pp->tid.p = NULL;
		vp->pp->tid.x = 0;

		vp->is_stop = FALSE;
		vp->is_finished = FALSE;
		vp->current_count = 0;
		vp->yuv_frame = NULL;
		vp->yuv_buf = NULL;
		vp->yuv_size = 0;
		vp->sws_ctx = NULL;
		vp->notification = NULL;
		vp->current_ctx = NULL;
		pthread_mutex_init(&vp->pp->mutex_t, NULL);
		pthread_cond_init(&vp->pp->cond_t, NULL);

		vp->progress = 0;
		LOG_PRINT("%s....width = %d, height = %d, fps = %d, format = %s, outpath = %s, audio_exit = %d", __FUNCTION__,
			vp->width, vp->height, fps, vp->format, vp->path, vp->audio_exist);
		LOG_PRINT("%s...vp->fps = %d", __FUNCTION__, vp->fps);
		/*void *call_back = &encode_callback_fun;
		vp->notification = call_back;*/

		int ret;
		/* 分配packet内存 */
		vp->pkt = av_packet_alloc();
		//AVPacket pkt;  
		AVCodec *codec = NULL;
		AVDictionary* dict = NULL;

		//分配给fifo_buf的大小：10帧  
		vp->fifo_buf = av_fifo_alloc(width * height * 4 * 10);
		if (vp->fifo_buf == NULL)
		{
			LOG_PRINT("%s....av_fifo_alloc failed", __FUNCTION__);
			ERR_NULL_EXIT
		}

		avcodec_register_all();
		av_register_all();

		char tmp_name[200];
		/* avformat_alloc_output_context2 只用format_name判断格式时，当format_name = mkv和wav时会判断不了*/
		if (!strcmp("alpha_mov", vp->format))
			sprintf_s(tmp_name, 200, "guess.mov");
		else
			sprintf_s(tmp_name, 200, "guess.%s", vp->format);
		AVOutputFormat *opFmt = av_guess_format(NULL, tmp_name, NULL);
		/* vp->fps < 0 indicates that there is no audio input */

		/* 音频存在则创建临时文件 */
		if (!audio_exist) {
			//vp->fps = -fps;  
			vp->tmp_path = NULL;
			ret = avformat_alloc_output_context2(&vp->fmt_ctx, opFmt, vp->format, vp->path);
		}
		else {
			//vp->fps = fps;  
			vp->tmp_path = EncodeCreateTempFile(vp->format);  //must free this variable ahead of freeing of Structure  
			if (vp->tmp_path == NULL) {
				LOG_PRINT("%s....CreateTempFile failed", __FUNCTION__);
				ERR_NULL_EXIT
			}
			ret = avformat_alloc_output_context2(&vp->fmt_ctx, opFmt, vp->format, vp->tmp_path);
		}
		if (ret < 0)
		{
			LOG_PRINT("%s....avformat_alloc_output_context2 failed", __FUNCTION__);
			ERR_NULL_EXIT
		}

		vp->stream = avformat_new_stream(vp->fmt_ctx, NULL);
		if (vp->stream == NULL)
		{
			LOG_PRINT("%s....avformat_new_stream failed", __FUNCTION__);
			ERR_NULL_EXIT
		}
		vp->codec_ctx = avcodec_alloc_context3(NULL);
		if (vp->codec_ctx == NULL)
		{
			LOG_PRINT("%s....avcodec_alloc_context3 failed", __FUNCTION__);
			ERR_NULL_EXIT
		}
		//vp->codec_ctx->b_frame_strategy  
		/* 选择encoder和pix_fmt */
		codecCtx_fill(vp->codec_ctx, &dict, format);
		vp->codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
		vp->codec_ctx->width = vp->width;
		vp->codec_ctx->height = vp->height;

		vp->codec_ctx->time_base.num = 1;
		vp->codec_ctx->time_base.den = vp->fps;

		//vp->stream->time_base = { 1, vp->fps };  
		vp->stream->time_base.num = 1;
		vp->stream->time_base.den = vp->fps;

		/* 查找编码器 */
		codec = avcodec_find_encoder(vp->codec_ctx->codec_id);
		if (codec == NULL)
		{
			LOG_PRINT("%s....avcodec_find_encoder failed", __FUNCTION__);
			ERR_NULL_EXIT
		}

		vp->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		/* 打开编码器 */
		ret = avcodec_open2(vp->codec_ctx, codec, &dict);
		if (ret != 0)
		{
			LOG_PRINT("%s....avcodec_open2 failed", __FUNCTION__);
			ERR_NULL_EXIT
		}
		/* copy the stream parameters to the muxer */
		ret = avcodec_parameters_from_context(vp->stream->codecpar, vp->codec_ctx);
		if (ret < 0)
		{
			LOG_PRINT("%s....avcodec_parameters_from_context failed", __FUNCTION__);
			ERR_NULL_EXIT
		}

		/* 填充argb_frame和yuv_frame(如果有) */
		if (frame_buf_alloc(vp, width, height)) {
			LOG_PRINT("%s....frame_buf_alloc failed", __FUNCTION__);
			ERR_NULL_EXIT
		}

#ifdef DEBUG_  
		//打印信息  
		if (vp->tmp_path == NULL)
			av_dump_format(vp->fmt_ctx, 0, vp->path, 1);
		else
			av_dump_format(vp->fmt_ctx, 0, vp->tmp_path, 1);
#endif  

		/* 读取输出文件到缓存中，接下来要对其进行写入 */
		if (vp->tmp_path == NULL)
			ret = avio_open(&vp->fmt_ctx->pb, vp->path, AVIO_FLAG_WRITE);
		else
			ret = avio_open(&vp->fmt_ctx->pb, vp->tmp_path, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			LOG_PRINT("%s....avio_open failed", __FUNCTION__);
			ERR_NULL_EXIT
		}

		/* 将帧的头部写入输出文件 */
		ret = avformat_write_header(vp->fmt_ctx, &dict);
		if (ret < 0)
		{
			LOG_PRINT("%s....avformat_write_header failed", __FUNCTION__);
			ERR_NULL_EXIT
		}

		LOG_PRINT("%s....Create an encode thread", __FUNCTION__);
		/* 创建编码线程 */
		pthread_create(&vp->pp->tid, NULL, encode_thread, vp);
		//线程退出时自动释放资源  
		pthread_detach(vp->pp->tid);
		return vp;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LOG_PRINT("%s error\n", __FUNCTION__);
	}
}

static void codecCtx_fill(AVCodecContext *codec_ctx, AVDictionary **dict, const char *out_fmt)
{
	int ret;
	/* 输出格式是mov（透明通道视频）*/
	if (!strcmp("alpha_mov", out_fmt)) {
		codec_ctx->pix_fmt = AV_PIX_FMT_ARGB;
	}
	else {
		codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	}
	if (!strcmp(out_fmt, "mp4") || !strcmp(out_fmt, "mkv") || !strcmp(out_fmt, "mov"))
	{
		codec_ctx->codec_id = AV_CODEC_ID_H264;
		codec_ctx->bit_rate = 800 * 1000;

		codec_ctx->rc_max_rate = 800 * 1000;
		codec_ctx->rc_buffer_size = 500 * 1000;
		/* 设置图像组层的大小, gop_size越大，文件越小 */
		codec_ctx->gop_size = 30;
		codec_ctx->max_b_frames = 3;
		/* 设置h264中相关的参数 */
		codec_ctx->qmin = 10;    //2  
		codec_ctx->qmax = 31;    //31  
		codec_ctx->max_qdiff = 4;
		codec_ctx->me_range = 16;    //0   
		codec_ctx->max_qdiff = 4;    //3   
		codec_ctx->qcompress = 0.6;  //0.5  
		ret = av_dict_set(dict, "profile", "high", 0);
		// 通过--preset的参数调节编码速度和质量的平衡。  
		ret = av_dict_set(dict, "preset", "superfast", 0);
		ret = av_dict_set(dict, "threads", "0", 0);
		ret = av_dict_set(dict, "crf", "26", 0);
		// zerolatency: 零延迟，用在需要非常低的延迟的情况下，比如电视电话会议的编码  
		ret = av_dict_set(dict, "tune", "zerolatency", 0);
		return;
	}
	else {
		codec_ctx->bit_rate = 4096 * 1000;
	}

	if (!strcmp(out_fmt, "avi")) {
		codec_ctx->codec_id = AV_CODEC_ID_MPEG4;
	}
	//未确定是V3  
	else if (!strcmp(out_fmt, "wmv")) {
		codec_ctx->codec_id = AV_CODEC_ID_MSMPEG4V3;
	}
	else if (!strcmp(out_fmt, "flv")) {
		codec_ctx->codec_id = AV_CODEC_ID_FLV1;
	}
	else if (!strcmp(out_fmt, "alpha_mov")) {
		codec_ctx->codec_id = AV_CODEC_ID_QTRLE;
	}
	/* 未找到对应格式时的默认选择 */
	else {
		codec_ctx->codec_id = AV_CODEC_ID_MPEG4;
	}
}

static int frame_buf_alloc(videoParm *vp, int width, int height)
{
	int raw_pix_fmt = AV_PIX_FMT_BGRA;
	/* 分配ARGB帧内存 */
	vp->argb_frame = av_frame_alloc();
	if (vp->argb_frame == NULL)
	{
		LOG_PRINT("%s....argb_frame alloc failed", __FUNCTION__);
		return -1;
	}
	/* 如果不是透明通道，则分配YUV帧内存 */
	if (strcmp(vp->format, "alpha_mov")) {
		vp->yuv_frame = av_frame_alloc();
		if (vp->yuv_frame == NULL) {
			LOG_PRINT("%s....yuv_frame alloc failed", __FUNCTION__);
			return -1;
		}
	}
	//av_frame_get_buffer  
	vp->argb_size = av_image_get_buffer_size((AVPixelFormat)raw_pix_fmt, width, height, 1);
	/* 为ARGB帧的data buffers分配内存 */
	vp->argb_buf = (uint8_t*)av_malloc(vp->argb_size);

	/* 如果不是透明通道，则为YUV帧的data buffers分配内存 */
	if (strcmp(vp->format, "alpha_mov")) {
		vp->yuv_size = av_image_get_buffer_size(vp->codec_ctx->pix_fmt, width, height, 1);
		vp->yuv_buf = (uint8_t*)av_malloc(vp->yuv_size);
	}

	//先让data指针和linesize指针指向buf，后面再从文件中写入数据到buf  
	/* 填充argb_frame */
	av_image_fill_arrays(vp->argb_frame->data, vp->argb_frame->linesize, vp->argb_buf, (AVPixelFormat)raw_pix_fmt, vp->width, vp->height, 1);
	/* 如果不是透明通道，则填充yuv_frame */
	if (strcmp(vp->format, "alpha_mov")) {
		av_image_fill_arrays(vp->yuv_frame->data, vp->yuv_frame->linesize, vp->yuv_buf, vp->codec_ctx->pix_fmt, width, height, 1);
	}
	/* 如果不是透明通道，获取SwsContext */
	//SWS_FAST_BILINEAR  SWS_BICUBIC  
	if (strcmp(vp->format, "alpha_mov")) {
		vp->sws_ctx = sws_getContext(width, height, (AVPixelFormat)raw_pix_fmt, width, height, vp->codec_ctx->pix_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);
	}
	return 0;
}

/*
* 外部传来的是BGRA帧，而AV_CODEC_ID_QTRLE这个编码器不支持AV_PIX_FMT_BGRA，所以需要转为ARGB帧
*/
void BGRA2ARGB(videoParm *vp)
{
	char B1, G1, R1, A1, B2, G2, R2, A2;
	int cnt = vp->argb_size / 8;
	uint8_t *p = vp->argb_buf;
	for (int i = 0; i < cnt; ++i) {
		B1 = *p;
		G1 = *(p + 1);
		R1 = *(p + 2);
		A1 = *(p + 3);
		B2 = *(p + 4);
		G2 = *(p + 5);
		R2 = *(p + 6);
		A2 = *(p + 7);
		*p = A1;
		*(p + 1) = R1;
		*(p + 2) = G1;
		*(p + 3) = B1;
		*(p + 4) = A2;
		*(p + 5) = R2;
		*(p + 6) = G2;
		*(p + 7) = B2;
		p += 8;
	}
	/* 如果argb_size多4个字节 */
	if (vp->argb_size % 8) {
		//p += 8;  
		B1 = *p;
		G1 = *(p + 1);
		R1 = *(p + 2);
		A1 = *(p + 3);
		*p = A1;
		*(p + 1) = R1;
		*(p + 2) = G1;
		*(p + 3) = B1;
	}
}

/*
* 外部传进来的是倒立的帧，需要进行翻转（BGRA转ARGB）
*/
int vertical_flip(videoParm *vp)
{
	size_t w = vp->width * 4;
	size_t h = vp->height;
	size_t m = h / 2;
	uint8_t *top = vp->argb_buf;
	uint8_t *bottom = vp->argb_buf + w * (h - 1);
	uint8_t *tmp_buf = (uint8_t*)av_malloc(w);
	if (NULL == tmp_buf) {
		LOG_PRINT("%s....av_malloc failed", __FUNCTION__);
		return -1;
	}

	for (int i = 0; i < m; ++i) {
		memcpy(tmp_buf, top, w);
		memcpy(top, bottom, w);
		memcpy(bottom, tmp_buf, w);
		top += w;
		bottom -= w;
	}
	if (tmp_buf) {
		av_free(tmp_buf);
	}
	return 0;
}

int pushing_frame(void *rawdatabuf, void *videoparam)
{
	__try {
		int ret = -1;
		int ret_size = -1;
		//int space = -1;  
		bool exit_flag = FALSE;
		videoParm *vp = (videoParm*)videoparam;

		/* 如果fifo可读空间>=10帧，停止push */
		pthread_mutex_lock(&vp->pp->mutex_t);
		if ((ret_size = av_fifo_size(vp->fifo_buf)) >= (vp->argb_size * 10)) {
			exit_flag = TRUE;
		}
		pthread_mutex_unlock(&vp->pp->mutex_t);
		if (exit_flag) {
			LOG_PRINT("%s....Wait for a minute to pushing_frame!!!", __FUNCTION__);
			return 0;
		}

		/* AVFifoBuf不到10帧，因此向其中写入帧 */
		pthread_mutex_lock(&vp->pp->mutex_t);
		ret = av_fifo_generic_write(vp->fifo_buf, rawdatabuf, vp->argb_size, NULL);

		pthread_cond_signal(&vp->pp->cond_t);
		pthread_mutex_unlock(&vp->pp->mutex_t);

#ifdef DEBUG_  
		++g_push_cnt;
#endif  
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LOG_PRINT("%s error\n", __FUNCTION__);
	}
	return 1;
}

static void* encode_thread(void *videoparm)
{
	__try {

		int ret = -1;
		bool flag;
		bool if_finished = FALSE;
		int status = -1;
		videoParm *vp = (videoParm*)videoparm;

		if (videoparm == NULL) {
			LOG_PRINT("%s....videoparm is NULL", __FUNCTION__);
			callback_function((EncodeCallbackFunction)vp->notification, vp, status, 0);
			ERR_NULL_EXIT
		}
		while (1) {
			flag = FALSE;
			pthread_mutex_lock(&vp->pp->mutex_t);
			/* 手动停止 */
			if (vp->is_stop) {
				flag = TRUE;
				status = 1;
			}
			/* 外部没有帧了 */
			if (vp->is_finished) {
				if_finished = TRUE;
				status = 0;
			}
			pthread_mutex_unlock(&vp->pp->mutex_t);
			if (flag || if_finished) {
				break;
			}
			/* 如果fifo_buf中没有帧可以读，那就Sleep */
			pthread_mutex_lock(&vp->pp->mutex_t);
			if (av_fifo_size(vp->fifo_buf) < vp->argb_size) {
				flag = TRUE;
			}
			else {
				flag = FALSE;
			}
			pthread_mutex_unlock(&vp->pp->mutex_t);
			if (flag) {
				LOG_PRINT("%s....Sleep 1 s", __FUNCTION__);
				encode_delay_time(&vp->pp->mutex_t, &vp->pp->cond_t, 1000);
				continue;
			}
			pthread_mutex_lock(&vp->pp->mutex_t);
			av_fifo_generic_read(vp->fifo_buf, vp->argb_buf, vp->argb_size, NULL);
			int size = av_fifo_size(vp->fifo_buf);
			pthread_mutex_unlock(&vp->pp->mutex_t);

#ifdef DEBUG_  
			DWORD dwStart = timeGetTime();
#endif  
			/* 垂直翻转图像 */
			/**/
			if (vertical_flip(vp)) {
				ERR_NULL_EXIT
			}

#ifdef DEBUG_  
			DWORD dwEnd = timeGetTime();
			printf("The duration is %lu milliseconds\n", dwEnd - dwStart);
#endif  

			if (!strcmp("alpha_mov", vp->format)) {
				BGRA2ARGB(vp);
			}

			/* 如果不是透明通道，RGB帧转YUV帧 */
			if (vp->yuv_frame != NULL) {
				sws_scale(vp->sws_ctx, vp->argb_frame->data, vp->argb_frame->linesize, 0, vp->height, vp->yuv_frame->data, vp->yuv_frame->linesize);
				vp->yuv_frame->pts = vp->current_count * (vp->stream->time_base.den) / ((vp->stream->time_base.num) * vp->fps);
				vp->yuv_frame->format = vp->codec_ctx->pix_fmt;
				vp->yuv_frame->width = vp->width;
				vp->yuv_frame->height = vp->height;
				++vp->current_count;
			}
			else {
				vp->argb_frame->pts = vp->current_count * (vp->stream->time_base.den) / ((vp->stream->time_base.num) * vp->fps);
				vp->argb_frame->format = vp->codec_ctx->pix_fmt;
				vp->argb_frame->width = vp->width;
				vp->argb_frame->height = vp->height;
				++vp->current_count;
			}

			int send_flag = -1, recv_flag = -1;
			/* 如果不是透明通道，为packet的data buffer 分配内存 */

			if (vp->yuv_size != 0) {
				av_new_packet(vp->pkt, vp->yuv_size);
			}
			else {
				av_new_packet(vp->pkt, vp->argb_size);
			}

			/* 如果不是透明通道 */
			if (vp->yuv_frame != NULL) {
				send_flag = avcodec_send_frame(vp->codec_ctx, vp->yuv_frame);
			}
			else {
				send_flag = avcodec_send_frame(vp->codec_ctx, vp->argb_frame);
			}
			if (send_flag < 0) {
				LOG_PRINT("%s....Avcodec_send_frame failed", __FUNCTION__);
			}
			recv_flag = avcodec_receive_packet(vp->codec_ctx, vp->pkt);
			if (recv_flag < 0) {
				LOG_PRINT("%s....avcodec_receive_packet failed", __FUNCTION__);
			}
			if (send_flag != 0 || recv_flag != 0) {
				av_packet_unref(vp->pkt);
				continue;
			}

			vp->pkt->stream_index = vp->stream->index;
			/* 将帧包写入输出文件 */
			ret = av_write_frame(vp->fmt_ctx, vp->pkt);
			/* 将packet的值还原为默认值 */
			av_packet_unref(vp->pkt);
			if (ret < 0) {
				LOG_PRINT("%s....av_write_frame failed", __FUNCTION__);
				ERR_NULL_EXIT
			}
			else if (ret == 0) {
#ifdef DEBUG_  
				++g_write_frame_cnt;
#endif  
			}

			encode_delay_time(&vp->pp->mutex_t, &vp->pp->cond_t, 10);
		}

#ifdef DEBUG_  
		LOG_PRINT("%s....av_write_frame succeed %d times", __FUNCTION__, g_write_frame_cnt);
#endif  

		/*
		* end_push后将fifo buf中剩余的帧写入文件
		*/
		while (1) {
			flag = FALSE;
			pthread_mutex_lock(&vp->pp->mutex_t);
			if (vp->is_stop) {
				flag = TRUE;
			}
			pthread_mutex_unlock(&vp->pp->mutex_t);
			if (flag) {
				break;
			}
			pthread_mutex_lock(&vp->pp->mutex_t);
			if ((ret = av_fifo_size(vp->fifo_buf)) < vp->argb_size) {
				flag = FALSE;
			}
			else {
				flag = TRUE;
			}
			pthread_mutex_unlock(&vp->pp->mutex_t);
			if (!flag) {
				break;
			}
			else {
				//编码下一帧  
				if (encode_remaining(vp)) {
					ERR_NULL_EXIT
				}
			}
		}

#ifdef DEBUG_  
		LOG_PRINT("%s....encode_remaining， av_write_frame succeed %d times", __FUNCTION__, g_write_frame_cnt);
#endif  

		pthread_mutex_lock(&vp->pp->mutex_t);
		if (vp->is_stop) {
			flag = TRUE;
		}
		else {
			flag = FALSE;
		}
		pthread_mutex_unlock(&vp->pp->mutex_t);
		if (!flag) {
			/* 如果没有push_frame过，就不flush */
			if (ret != -1) {
				flush_encode(vp);
			}
		}

#ifdef DEBUG_  
		LOG_PRINT("%s....flush_encode， av_write_frame succeed %d times", __FUNCTION__, g_write_frame_cnt);
#endif  

		/* 将流尾写入输出文件并释放文件 */
		av_write_trailer(vp->fmt_ctx);

		callback_function((EncodeCallbackFunction)vp->notification, vp, status, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LOG_PRINT("%s error\n", __FUNCTION__);
	}
	return NULL;
}

static int encode_remaining(videoParm *vp)
{
	static int s_frame_cnt = 1;
	pthread_mutex_lock(&vp->pp->mutex_t);
	av_fifo_generic_read(vp->fifo_buf, vp->argb_buf, vp->argb_size, NULL);
	pthread_mutex_unlock(&vp->pp->mutex_t);

	/* 垂直翻转图像 */
	if (vertical_flip(vp)) {
		return -1;
	}
	if (!strcmp("alpha_mov", vp->format)) {
		BGRA2ARGB(vp);
	}

	/* RGB帧转YUV帧 */
	/* 如果不是透明通道 */
	if (vp->yuv_frame != NULL) {
		sws_scale(vp->sws_ctx, vp->argb_frame->data, vp->argb_frame->linesize, 0, vp->height, vp->yuv_frame->data, vp->yuv_frame->linesize);

		vp->yuv_frame->pts = vp->current_count * (vp->stream->time_base.den) / ((vp->stream->time_base.num) * vp->fps);
		//vp->yuv_frame->pts = vp->current_count * vp->codec_ctx->time_base.den / (vp->codec_ctx->time_base.num * vp->fps);  
		vp->yuv_frame->format = vp->codec_ctx->pix_fmt;
		vp->yuv_frame->width = vp->width;
		vp->yuv_frame->height = vp->height;
		++vp->current_count;
	}
	else {
		vp->argb_frame->pts = vp->current_count * (vp->stream->time_base.den) / ((vp->stream->time_base.num) * vp->fps);
		//vp->argb_frame->pts = vp->current_count * vp->codec_ctx->time_base.den / (vp->codec_ctx->time_base.num * vp->fps);  
		vp->argb_frame->format = vp->codec_ctx->pix_fmt;
		vp->argb_frame->width = vp->width;
		vp->argb_frame->height = vp->height;
		++vp->current_count;
	}
	int send_flag = -1, recv_flag = -1;
	/* 为packet的data buffer 分配内存 */
	if (vp->yuv_size != 0) {
		av_new_packet(vp->pkt, vp->yuv_size);
	}
	else {
		av_new_packet(vp->pkt, vp->argb_size);
	}

	/* 如果不是透明通道 */
	if (vp->yuv_frame != NULL) {
		send_flag = avcodec_send_frame(vp->codec_ctx, vp->yuv_frame);
	}
	else {
		send_flag = avcodec_send_frame(vp->codec_ctx, vp->argb_frame);    //46ms,49ms,152ms  
	}
	if (send_flag < 0) {
		LOG_PRINT("%s....Avcodec_send_frame failed", __FUNCTION__);
	}
	recv_flag = avcodec_receive_packet(vp->codec_ctx, vp->pkt);
	//编码失败不写入文件，跳到下一帧  
	if (send_flag != 0 || recv_flag != 0) {
		av_packet_unref(vp->pkt);
		return 0;
	}
	vp->pkt->stream_index = vp->stream->index;
	/* 将帧包写入输出文件 */
	int ret = av_write_frame(vp->fmt_ctx, vp->pkt);   //21ms,8ms  
	if (ret < 0) {
		LOG_PRINT("%s....av_write_frame failed", __FUNCTION__);
	}
	else if (ret == 0) {
#ifdef DEBUG_  
		++g_write_frame_cnt;
		printf("fifo av_write_frame succeed %3d times\n", s_frame_cnt++);
#endif  
		LOG_PRINT("%s....fifo av_write_frame succeed", __FUNCTION__);
	}
	/* 将packet的值还原为默认值 */
	av_packet_unref(vp->pkt);
	return 0;
}

static void flush_encode(videoParm *vp)
{
#ifdef DEBUG_  
	LOG_PRINT("%s....g_push_cnt = %d", __FUNCTION__, g_push_cnt);
#endif  

	int flush_frame_cnt = 1;
	int ret = avcodec_send_frame(vp->codec_ctx, NULL);
	while (1) {
		av_new_packet(vp->pkt, vp->argb_size);
		vp->pkt->data = NULL;
		vp->pkt->size = 0;
		ret = avcodec_receive_packet(vp->codec_ctx, vp->pkt);
		if (ret == AVERROR_EOF) {
			break;  //break之后忘记了av_packet_unref，参考encode_audio_frame_flush  
		}
		else if (ret == 0) {
			vp->pkt->stream_index = vp->stream->index;
			ret = av_write_frame(vp->fmt_ctx, vp->pkt);   //21ms,8ms  
			if (ret == 0) {
#ifdef DEBUG_  
				++g_write_frame_cnt;
				printf("flush frame succeed %3d times\n", flush_frame_cnt++);
#endif  
				LOG_PRINT("%s....flush frame succeed", __FUNCTION__);
			}
			else if (ret < 0) {
				LOG_PRINT("%s....av_write_frame failed", __FUNCTION__);
			}
			else {  //1  
				LOG_PRINT("%s....flushed and there is no more data to flush", __FUNCTION__);
			}
		}
		av_packet_unref(vp->pkt);

		Sleep(10);
	}
	av_packet_unref(vp->pkt);
}

void ending_push(void *videoparam)
{
	videoParm *vp = (videoParm*)videoparam;
	pthread_mutex_lock(&vp->pp->mutex_t);
	vp->is_finished = TRUE;
	pthread_mutex_unlock(&vp->pp->mutex_t);
}

void releasing(videoParm *vp)
{
	__try {
		int err;
		/* 释放锁 */
		pthread_mutex_destroy(&vp->pp->mutex_t);
		pthread_cond_destroy(&vp->pp->cond_t);
		if (vp->format) {
			free((void*)vp->format);
		}
		if (vp->path) {
			free((void*)vp->path);
		}

		/* 释放Sws_Context结构体 */
		if (vp->sws_ctx != NULL) {
			sws_freeContext(vp->sws_ctx);
		}
		/* 释放AVFifoBuffer结构体 */
		av_fifo_free(vp->fifo_buf);
		/* 释放ARGB帧和YUV帧结构 */
		av_frame_free(&vp->argb_frame);
		if (vp->yuv_frame != NULL) {
			av_frame_free(&vp->yuv_frame);
		}
		/* 释放ARGB帧和YUV帧内存 */
		av_free(vp->argb_buf);
		if (vp->yuv_buf != NULL) {
			av_free(vp->yuv_buf);
		}
		/* 释放packet结构体*/
		av_packet_free(&vp->pkt);
		/* 释放AVCodecContext结构体 */
		avcodec_free_context(&vp->codec_ctx);
		//avcodec_close(vp->codec_ctx);  

		/* 关闭AVIOContext */
		avio_close(vp->fmt_ctx->pb);
		/* 必须先调用avio_close关闭了与输出文件的关联，才能再调用remove删除输出文件 */
		if (vp->tmp_path) {
#ifdef REMOVE_TEMP_FILE  
			err = remove(vp->tmp_path);
			if (!err) {
				LOG_PRINT("%s....Remove temp file succeed", __FUNCTION__);
			}
			else {
				DWORD tt = GetLastError();
				LOG_PRINT("%s....Remove temp file failed, error code is %d", __FUNCTION__, err);
			}
#endif  
			free(vp->tmp_path);
		}
		/* 释放AVFormatContext结构体 */
		avformat_free_context(vp->fmt_ctx);
		vp->fmt_ctx = NULL;

		/* 释放videoParm结构体 */
		av_free(vp);
		/* 设为NULL，避免使用野指针 */
		vp = NULL;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		LOG_PRINT("%s error\n", __FUNCTION__);
	}
}

void setting_stop(void *videoparm)
{
	videoParm *vp = (videoParm*)(videoparm);
	pthread_mutex_lock(&vp->pp->mutex_t);
	vp->is_stop = TRUE;
	pthread_mutex_unlock(&vp->pp->mutex_t);
}

static void callback_function(EncodeCallbackFunction fun, void *identifier, int status, int percent)
{
	if (fun != NULL) {
		fun(identifier, status, percent);
	}
}

/* ANSI */
static char* EncodeCreateTempFile(const char *extention)
{
	const char *ext;
	if (!strcmp("alpha_mov", extention))
		ext = "mov";
	else
		ext = extention;
	char *path = EncodeCreateTempDir("EnocdeTemp");
	if (path == NULL) {
		LOG_PRINT("%s....Create temp file failed", __FUNCTION__);
		return NULL;
	}

	char *filename = (char*)malloc(1024);
	memset(filename, 0, 1024);
	sprintf(filename, "%s/ENC%lld.%s", path, (int64_t)av_gettime(), ext);
	LOG_PRINT("%s....The temp file in path: %s", __FUNCTION__, filename);
	free(path);

	return filename;
}

static void encode_delay_time(pthread_mutex_t *p_mutex, pthread_cond_t *p_cond, uint64_t ms)
{
#ifndef ETIMEDOUT  
#define ETIMEDOUT   60  
#endif  
	struct timespec m_time;
	int64_t tmp_us = av_gettime(); //usec      
	uint64_t current_us = tmp_us;
	if (tmp_us<0)
	{
		current_us = (tmp_us - 1) ^ ((int64_t)(-1));
	}
	current_us += ms * 1000;
	m_time.tv_sec = current_us / 1000000;
	m_time.tv_nsec = (current_us % 1000000) * 1000;
	pthread_mutex_lock(p_mutex);
	int res = pthread_cond_timedwait(p_cond, p_mutex, (const struct timespec *)&m_time);
	pthread_mutex_unlock(p_mutex);
	//sleep(1);  
	if (res == ETIMEDOUT)//timeout  
	{
		/* 超时 */
	}
	else
	{
		/* 被信号唤醒 */
	}
}

/* ANSI */
static char *EncodeCreateTempDir(char *sub_dir)
{
	char *path = (char *)malloc(1024);
	memset(path, 0, 1024);
#ifdef _WIN32  
	char *tmp_path = getenv("TEMP");
#else  
	char *tmp_path = getenv("TMPDIR");
#endif  
	sprintf(path, "%s", tmp_path);
	if (strlen(path) == 0) {
		free(path);
		return NULL;
	}
	if (sub_dir&&strlen(sub_dir))
	{
		char chr = path[strlen(path) - 1];
		if (chr != '/'&&chr != '\\') {
			memcpy(path + strlen(path), "/", 1);
		}
		char *tmp = NULL;
		while ((tmp = strrchr(path, '\\')))
		{
			*tmp = '/';
		}
		memcpy(path + strlen(path), sub_dir, strlen(sub_dir));
	}
	if (access(path, 0)<0) {
#ifdef _WIN32  
		int ret = mkdir(path);
		if (ret<0)
		{
			LOG_PRINT("%s....mkdir failed", __FUNCTION__);
			printf("failed to create tmp!\n");
		}
#else  
		char command[1024] = { 0 };
		sprintf(command, "mkdir %s", path);
		system(command);
#endif  

	}
	if (access(path, 0)<0) {
		//failed  
		LOG_PRINT("%s....access failed", __FUNCTION__);
		free(path);
		return NULL;
	}
	return path;
}
