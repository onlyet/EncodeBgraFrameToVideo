#ifndef MUX_H  
#define MUX_H  

#ifdef __cplusplus  
extern "C"
{
#endif  

#include"common.h"  
#include"encode.h"  

	typedef struct AVMuxing {
		videoParm *vp;
		AVFormatContext *i_fmt_ctx_v;
		AVFormatContext *i_fmt_ctx_a;
		AVFormatContext *o_fmt_ctx;
		AVCodecContext *enc_ctx_v;
		AVCodecContext *enc_ctx_a;
		const char *in_audio;

		int audio_len;
		int videoindex_v;
		int videoindex_out;
		int audioindex_a;
		int audioindex_out;
	}AVMuxing;

	/*
	*identifier,status, percent
	*/
	typedef void(*AVMuxCallbackFunction)(void*, int, int);

	/*
	* 输入编码时返回的结构体指针，一个音频
	*/
	extern int MuxAudioVideo(videoParm *vp, const char *inAudio);

#ifdef __cplusplus  
};
#endif  
#endif  
