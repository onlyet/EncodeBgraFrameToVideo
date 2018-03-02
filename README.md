# EncodeBgraFrameToVideo
### 将外部传进来的BGRA帧编码成各种格式的视频文件

这是某动画软件的一个后台视频编码项目。

该项目基于FFFmpeg框架，将air平台传来的每一BGRA帧进行编码，生成视频文件，然后将视频和air平台传来的MP3文件重新复用（mux），生成各种格式的视频。

* common.h  
  > 导入FFmpeg需要的头文件和库

* encode.h    
  > 对外（air平台）提供了5个接口:    
  >> preparing_to_push //设置视频参数和FFmpeg相关结构体，创建编码线程encode_thread  
  >> pushing_frame     //外部传入一帧（倒转的RGBA帧），将帧写入AVFifoBuffer中  
  >> ending_push       //当外部没有帧了，将is_finished标志位置1，用以通知编码线程要停止编码    
  >> setting_stop      //当用户按了停止键，将is_stop标志位置1，通知编码线程退出循坏，提前结束编码  
  >> releasing         //释放结构体动态分配的内存，锁资源，删除临时文件（如果有）  

* encode.cpp    
  > 编码的具体实现，生成某种格式的容器(mp4，avi，wmv，flv，mov，透明通道mov）  
  > 相关知识点或技术：多线程，条件变量，锁，回调函数    

* mux.h   
  > 保存之前编码的结构体videoParm，添加mux参数     
  
* mux.cpp   
  > 将encode生成的视频文件(无音频)和外部传来的音频文件(.mp3）remux成新的视频文件   
  
### 总结
部分代码冗余， 缺乏可重用性，逻辑不够清晰，可阅读性不高，很多可以优化的地方。
