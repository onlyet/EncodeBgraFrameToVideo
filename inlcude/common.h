#ifdef __cplusplus  
extern "C"
{
#endif  

#include "pthread.h"  
#include "libavformat/avformat.h"  
#include "libswscale/swscale.h"  
#include "libavutil/avutil.h"  
#include "libavutil/imgutils.h"  
#include "libavutil/pixfmt.h"  
#include "libavutil/time.h"  
#include "libavfilter/avfilter.h"  

#pragma comment(lib,"pthreadVC2.lib")  
#pragma comment(lib,"avutil.lib")  
#pragma comment(lib,"swresample.lib")  
#pragma comment(lib,"avcodec.lib")  
#pragma comment(lib,"avformat.lib")  
#pragma comment(lib,"postproc.lib")  
#pragma comment(lib,"swscale.lib")  
#pragma comment(lib,"avfilter.lib")  

#ifdef __cplusplus  
};
#endif 
