# F1C200S fplayerdemo

`fplayerdemo` 是面向当前 F1C200S 板子的轻量播放器和解码诊断工具。
它使用开源 Cedrus/V4L2 request 解码链路播放 H.264 视频，通过 KMS/DRM
直接显示 Cedrus 输出的 tiled NV12 帧；音频由 FFmpeg 解码后写入 ALSA PCM。

它不是通用 FFmpeg 播放器，也不使用 GStreamer pipeline。容器和 H.264 元数据由
内置 parser 解析；FFmpeg 只负责 AAC 音频解码。

## 当前能力

### 视频

- 支持 H.264/AVC 硬件解码。
- 支持 MP4/MOV 容器中的 H.264 视频。
- 通过内置轻量 parser 解析 SPS/PPS/slice 元数据。
- 通过 V4L2 stateless request API 提交 H.264 slice；多 slice 帧使用
  `V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF` 保持同一 capture buffer。
- 通过 KMS plane 显示 Allwinner tiled NV12 capture buffer。

MP4 通过 mmap 按需读取当前 sample，不执行应用层预读，也不会在播放前把整部媒体
的 packet 复制到内存。

视频解码通过后端表分发，目前只注册 H.264/Cedrus。MPEG-2、MPEG-4、H.263 和
JPEG 尚未实现 parser 或 V4L2 解码入口，后续可作为独立后端加入。

### 音频

MP4/MOV 中的 AAC 音频通过 FFmpeg/libavcodec 解码，当前配置支持：

```text
AAC, AAC fixed-point
```

AAC 默认使用 `aac_fixed` 解码器，更适合 F1C200S 这类弱 CPU/软浮点环境。

`fplayerdemo` 不主动修改 ALSA mixer。播放前请用 `amixer` 或板级初始化脚本自行设置
音频通路和音量。

### 目录播放

目录模式会扫描以下后缀的普通文件并按路径排序：

```text
.mp4 .m4v .mov .m4a
```

## Buildroot 集成

在 Buildroot 菜单中启用：

```text
Target packages -> Audio and video applications -> fplayerdemo
```

当前 `configs/epass_f1c200s_defconfig` 已默认启用：

```text
BR2_PACKAGE_FPLAYERDEMO=y
```

`alsa-lib`、FFmpeg 库和 libdrm 由包依赖/select 关系提供。H.264 parser 直接编译进
`fplayerdemo`，不依赖 GStreamer、GLib 或 libstdc++。

单独重建播放器：

```sh
make fplayerdemo-rebuild
```

重建时会先用主机编译器运行内置 H.264 parser 自测，再交叉编译目标程序。

安装位置：

```text
/usr/bin/fplayerdemo
```

## 基本用法

自动播放单个文件：

```sh
fplayerdemo /media/video.mp4
```

播放目录：

```sh
fplayerdemo /media/
```

循环播放：

```sh
fplayerdemo -l /media/video.mp4
fplayerdemo -l /media/
```

只播放视频：

```sh
fplayerdemo -K /media/video.mp4
```

只播放音频：

```sh
fplayerdemo -E /media/audio.m4a
```

显式音视频同播：

```sh
fplayerdemo -K -E /media/video.mp4
```

## 选项

```text
-K        只播放视频，使用 KMS/Cedrus
-E        只播放音频；可与 -K 组合为显式音视频播放
-l        循环播放输入文件或目录播放列表
-B        benchmark/no pace；音频场景下只解码，不写 ALSA
-Z        裁剪到屏幕尺寸，避免显示引擎缩放
-U        每秒输出 FPS、CPU、RSS 和 timing 统计
-P        播放结束时输出 profile 统计
-L        禁用 atomic KMS，使用 legacy SETPLANE
-X name   选择音频解码后端
-p id     指定 KMS plane id
-o dev    指定 ALSA PCM 设备，默认 hw:0,0
```

可用音频后端：

```text
auto
ffmpeg / avcodec / ffmpeg-fixed / aac_fixed / avcodec-fixed
ffmpeg-float / avcodec-float
```

常用调试命令：

```sh
fplayerdemo -K -U /media/video.mp4
fplayerdemo -K -B -P /media/video.mp4
fplayerdemo -E -B -P -X ffmpeg-float /media/audio.m4a
```

播放器不调用 `MADV_WILLNEED`、`MADV_POPULATE_READ`、后台 page-touch 或独立
`pread` 窗口来预读文件。原生 MP4 direct-mmap 路径只在消费者访问当前 sample 时
按需缺页；纯视频播放可对已播放且不再需要的旧页调用 `MADV_DONTNEED`。

普通 mmap 缺页仍会经过 Linux 页缓存及其常规缓存策略。

## FFmpeg 配置说明

当前 FFmpeg 只构建库，不构建命令行程序：

```text
ffmpeg/ffplay/ffprobe: disabled
network/avdevice/avformat: disabled
encoders/muxers/demuxers/parsers/filters/bsfs/protocols: disabled
```

FFmpeg 只保留 AAC 解码器，不启用 H.264 软件解码器。MP4/H.264 解析由内置 parser
完成，视频解码由 Cedrus 完成。

## 限制

- 视频编码只支持 H.264。
- 容器只支持 MP4/MOV，不支持裸 H.264 或其他容器。
- MPEG-2、H.265、VP8/VP9、MPEG-4、WMV、MJPEG 等视频编码不会播放。
- 不提供软件旋转选项；显示方向依赖 KMS plane 能力和当前自动适配逻辑。
- 不主动配置 ALSA mixer。
- 输入必须是本地文件或目录，不支持网络 URL。
