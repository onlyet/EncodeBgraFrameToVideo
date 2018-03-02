#ifdef __cplusplus  
extern "C"
{
#endif  

#include"common.h"    
#include"libavutil\fifo.h"  

	typedef struct pthreadParm {
		/* 线程id */
		pthread_t tid;
		/* 互斥锁 */
		pthread_mutex_t mutex_t;
		/* 条件等待 */
		pthread_cond_t cond_t;
	}pthread_Parm;

	/* 视频参数结构体，方便传参数*/
	typedef struct videoParms
	{
		AVFormatContext *fmt_ctx;
		AVCodecContext *codec_ctx;
		AVStream *stream;
		SwsContext *sws_ctx;
		AVFrame *argb_frame;
		AVFrame *yuv_frame;
		int argb_size;
		int yuv_size;
		/* 输入的argb帧 */
		uint8_t *argb_buf;
		/* 如果不是透明通道，则需转为yuv帧 */
		uint8_t *yuv_buf;
		AVPacket *pkt;
		/* 帧宽度 */
		int width;
		/* 帧高度 */
		int height;
		/* 帧率 */
		int fps;
		/* 输出视频格式 */
		const char *format;
		/* 输出路径 */
		const char *path;
		/* 强制结束标志 */
		bool is_stop;
		/* 编码结束标志 */
		bool is_finished;
		/* 当前帧数 */
		int current_count;
		pthreadParm *pp;
		/* 写入缓存 */
		AVFifoBuffer *fifo_buf;
		/* 回调函数 */
		void *notification;
		/* FREContext */
		void *current_ctx;
		/* 临时路径 */
		char *tmp_path;
		/* 进度 */
		int progress;
		/* 音频有无标志，如果没有音频不生成临时文件，有音频要生成临时文件 */
		int audio_exist;
		AVBitStreamFilterContext *bsfc;
	}videoParm;

	/*
	*identifier,status,percent
	*/
	typedef void(*EncodeCallbackFunction)(void*, int, int);

	/* 输入宽，高，帧率，输出视频格式，输出路径，音频存在标志。
	* 该函数为结构体分配内存，然后创建编码线程，返回指向结构体的指针
	*/
	extern void *preparing_to_push(int width, int height, int fps, int audio_exist, const char *format, const char *path);

	/*
	* 输入结构体指针，外部传入一帧（倒转的RGBA帧），将帧写入AVFifoBuffer中
	*/
	extern int pushing_frame(void *rawdatabuf, void *videoparam);

	/*
	* 当外部没有帧了，将is_finished标志位置1，用以通知编码线程要停止编码了
	*/
	extern void ending_push(void *videoparam);

	/*
	* 当用户按了停止键，将is_stop标志位置1，通知编码线程推出循坏，提前结束编码
	*/
	extern void setting_stop(void *videoparm);

	/*
	* 释放结构体动态分配的内存，锁资源，删除临时文件（如果有）
	*/
	extern void releasing(videoParm *vp);

#ifdef __cplusplus  
};
#endif 
