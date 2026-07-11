// ── 项目头文件 ──
#include "QRCodeForStream.h"

// ── 标准库 ──
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>

// ── 项目模块 ──
#include "QRScanner.h"
#include "MhyApi.hpp"

// 直播流扫码的提交节奏（毫秒）。
// 直播流帧率高（30~60fps），若对每一帧都调用 threadPool.tryStart 提交 QR 解码，
// 2~3 个线程的线程池会被慢速 WeChatQRCode DNN 解码占满，tryStart 在无空闲线程时
// 静默返回 false 丢帧（含二维码帧），表现为“大概率无反应、偶尔能扫上”。
// 这里用「最新帧」机制 + 固定节奏解决：解码循环对每一帧都执行 sws_scale，但只在
// 节奏窗口打开时把【当前最新一帧】提交给线程池；窗口关闭期间的帧只从解码器排空、
// 并把最新一帧缓存下来，绝不直接丢弃。这样既保证二维码帧一定会被扫到，
// 又不会因提交过密压垮线程池。节奏与屏幕扫码路径（QRCodeForScreen 的 DELAYED=200）一致。
static constexpr auto kStreamSubmitInterval = std::chrono::milliseconds(200);
// 流卡死看门狗：超过此时长未读到任何一帧，判定直播流已中断并给出反馈。
static constexpr auto kStreamStallTimeout = std::chrono::seconds(10);

/**
 * @brief 构造函数：初始化 FFmpeg 上下文指针、日志级别与配置单例。
 *
 * 所有 FFmpeg 结构体指针默认置空，在 init() 中按需分配，
 * 析构 / run() 末尾统一释放，避免野指针。
 */
QRCodeForStream::QRCodeForStream(QObject* parent) :
    QThread(parent),
    pAvdictionary(nullptr),
    pAVFormatContext(nullptr),
    pSwsContext(nullptr),
    pAVFrame(nullptr),
    pAVPacket(nullptr),
    pAVCodecContext(nullptr),
    m_stop(false),
    servertype(ServerType::Official)

{
    // 关闭 FFmpeg 冗余日志，仅保留致命错误，避免刷屏。
    av_log_set_level(AV_LOG_FATAL);
    // 绑定全局配置单例（读取 auto_login 等开关）。
    m_config = &(ConfigDate::getInstance());
}

QRCodeForStream::~QRCodeForStream()
{
    // 确保解码线程安全退出：请求中断并等待线程结束，避免析构时线程仍在跑。
    // 注：m_stop 为 false 表示「停止」，此处先置位再请求中断，是为了与 run() 中
    // m_stop.store(true)（表示「运行」）的语义保持一致。
    if (!this->isInterruptionRequested())
    {
        m_stop.store(false);
    }
    this->requestInterruption();
    this->wait();
}

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view gameToken)
{
    this->uid = uid;
    this->gameToken = gameToken;
}

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view gameToken, const std::string& name)
{
    this->uid = uid;
    this->gameToken = gameToken;
    this->m_name = name;
}

void QRCodeForStream::setServerType(const ServerType servertype)
{
    this->servertype = servertype;
}

/**
 * @brief 官方服务器直播流扫码主循环（米游社官服 / B站官服二维码）
 *
 * 解码子线程入口之一。从 FFmpeg 读取视频包并解码，采用「最新帧」机制 +
 * 固定节奏（kStreamSubmitInterval）提交 QR 解码任务，彻底解决旧版
 * 「逐帧 tryStart 丢帧 → 大概率无反应、偶尔能扫上」的问题；
 * 同时内置无画面看门狗，流卡死时给出明确反馈而非原地空转。
 *
 * 数据流：av_read_frame → avcodec_send_packet → avcodec_receive_frame
 *         → sws_scale 转 BGR → 缓存最新帧 → 节奏限流后提交线程池解码。
 */
void QRCodeForStream::LoginOfficial()
{
    // 最新帧缓存：解码循环每解出一帧都刷新它；节奏窗口打开时，
    // 只把【当前最新一帧】交给线程池。窗口关闭期间的帧不会真正丢弃，
    // 而是继续刷新 latestFrame，从而保证二维码帧一定会被扫到。
    std::shared_ptr<cv::Mat> latestFrame;
    // 上一次成功提交解码的时间点，用于节奏限流。
    auto lastSubmit = std::chrono::steady_clock::now() - kStreamSubmitInterval;
    // 上一次成功解出视频帧的时间点，用于卡死看门狗。
    // 注意：只有真正解出视频帧时才刷新（见内层循环），而非每次读到数据包就刷新，
    // 否则网络层心跳包会让看门狗永远不触发（这正是旧版看门狗失效的根因）。
    auto lastFrameTime = std::chrono::steady_clock::now();

    while (m_stop.load())
    {
        // ── 卡死看门狗 ──
        // 距上一次成功解出视频帧已超过阈值，说明直播流已中断或长时间无画面，
        // 主动给出反馈并退出，避免线程原地空转、用户毫无感知。
        // 说明：若 av_read_frame 自身阻塞卡死（连接假死），需依赖下方 <0 分支或
        // ffmpeg 内部超时；本看门狗主要覆盖「有连接但持续无视频帧」的场景。
        if (std::chrono::steady_clock::now() - lastFrameTime > kStreamStallTimeout)
        {
            std::string error_msg = "直播流已中断或无画面数据（超过 "
                + std::to_string(kStreamStallTimeout.count()) + " 秒未收到视频帧）。"
                "请检查直播是否仍在进行、网络是否稳定。";
            std::cerr << "[FFmpeg] " << error_msg << std::endl;
            emit streamError(QString::fromStdString(error_msg));
            ret = ScanRet::LIVESTOP;
            break;
        }

        // 读取下一帧（AVPacket）。返回 < 0 表示流结束或网络断开。
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        // 只处理视频流，忽略音频 / 其他流的数据包。
        if (pAVPacket->stream_index != videoStreamIndex)
        {
            continue;
        }
        // 将压缩包送入解码器。
        avcodec_send_packet(pAVCodecContext, pAVPacket);
        if (pAVFrame == nullptr)
        {
            std::cerr << "Error allocating frame" << std::endl;
            ret = ScanRet::LIVESTOP;
            break;
        }

        // 一个数据包可能解出多帧，循环取尽。
        while (avcodec_receive_frame(pAVCodecContext, pAVFrame) == 0)
        {
            // 将解码后的原始帧转换为 BGR 的 cv::Mat，供 QR 识别使用。
            auto img = std::make_shared<cv::Mat>(videoStreamHeight, videoStreamWidth, CV_8UC3);
            uint8_t* dstData[1] = { img->data };
            const int dstLinesize[1] = { static_cast<int>(img->step) };
            sws_scale(pSwsContext, pAVFrame->data, pAVFrame->linesize, 0, pAVFrame->height,
                      dstData, dstLinesize);

            // 真正解出一帧 → 刷新看门狗时间戳（关键：仅在解出视频帧时刷新）。
            lastFrameTime = std::chrono::steady_clock::now();

#ifndef SHOW
            cv::imshow("Video_Stream", *img);
            cv::waitKey(1);
#endif
            // 始终缓存最新一帧（覆盖上一帧），窗口关闭期间也不会丢失。
            latestFrame = img;

            // ── 节奏限流 ──
            // 仅当距上次提交已超过 kStreamSubmitInterval 才提交一帧解码，
            // 其余帧仅刷新 latestFrame 后跳过，避免线程池被占满导致静默丢帧。
            const auto now = std::chrono::steady_clock::now();
            if (now - lastSubmit < kStreamSubmitInterval)
            {
                continue;
            }
            lastSubmit = now;

            // 以 shared_ptr 拷贝方式捕获最新帧：即使线程池此刻已满（tryStart 返回 false），
            // latestFrame 仍持有该帧，下一轮窗口打开时可再次提交，绝不丢帧。
            // （旧版用 std::move 捕获，tryStart 失败时该帧被永久丢弃。）
            threadPool.tryStart([&, latestFrame]() {
                thread_local QRScanner qrScanners;
                std::string str;
                qrScanners.decodeSingle(*latestFrame, str);
                // 米游社登录二维码解码结果长度约 85+，过短直接丢弃（非二维码或残缺）。
                if (str.size() < 85)
                {
                    return;
                }
                // 取 game type 标识位（第 80~82 字节）判断是否本程序支持的登录类型。
                std::string_view view(str.c_str() + 79, 3);
                if (!setGameType.contains(view))
                {
                    return;
                }
                // 取末尾 24 字节作为登录 ticket。
                const std::string_view ticket(str.data() + str.size() - 24, 24);
                // 通过回调设置当前 gameType（如原神 / 崩坏等）。
                setGameType[view]();
                // 同一个 ticket 已处理过则跳过，避免重复登录请求。
                if (lastTicket == ticket)
                {
                    return;
                }
                // 加锁后再次确认仍在运行，再发起登录请求。
                if (mtx.try_lock())
                {
                    if (!m_stop.load())
                    {
                        mtx.unlock();
                        return;
                    }
                    if (ScanQRLogin(scanUrl.data(), ticket, gameType))
                    {
                        lastTicket = ticket;
                        nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                        if (config["auto_login"])
                        {
                            continueLastLogin();
                        }
                        else
                        {
                            Q_EMIT loginConfirm(gameType, false);
                        }
                    }
                    else
                    {
                        Q_EMIT loginResults(ScanRet::FAILURE_1);
                    }
                    stop();
                    mtx.unlock();
                }
            });
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

/**
 * @brief 崩坏3 B站直播流扫码主循环
 *
 * 与 LoginOfficial 机制完全一致，仅 gameType 判定与登录校验函数不同：
 * 本服二维码固定以 "8F3" 标识，并使用 scanCheck 校验 ticket。
 */
void QRCodeForStream::LoginBH3BiliBili()
{
    // 最新帧缓存：解码循环每解出一帧都刷新它；节奏窗口打开时，
    // 只把【当前最新一帧】交给线程池。窗口关闭期间的帧不会真正丢弃。
    std::shared_ptr<cv::Mat> latestFrame;
    // 上一次成功提交解码的时间点，用于节奏限流。
    auto lastSubmit = std::chrono::steady_clock::now() - kStreamSubmitInterval;
    // 上一次成功解出视频帧的时间点，用于卡死看门狗（仅在解出视频帧时刷新）。
    auto lastFrameTime = std::chrono::steady_clock::now();

    while (m_stop.load())
    {
        // ── 卡死看门狗 ──
        if (std::chrono::steady_clock::now() - lastFrameTime > kStreamStallTimeout)
        {
            std::string error_msg = "直播流已中断或无画面数据（超过 "
                + std::to_string(kStreamStallTimeout.count()) + " 秒未收到视频帧）。"
                "请检查直播是否仍在进行、网络是否稳定。";
            std::cerr << "[FFmpeg] " << error_msg << std::endl;
            emit streamError(QString::fromStdString(error_msg));
            ret = ScanRet::LIVESTOP;
            break;
        }

        // 读取下一帧。返回 < 0 表示流结束或网络断开。
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        if (pAVPacket->stream_index != videoStreamIndex)
        {
            continue;
        }
        avcodec_send_packet(pAVCodecContext, pAVPacket);
        if (pAVFrame == nullptr)
        {
            std::cerr << "Error allocating frame" << std::endl;
            ret = ScanRet::LIVESTOP;
            break;
        }

        while (avcodec_receive_frame(pAVCodecContext, pAVFrame) == 0)
        {
            auto img = std::make_shared<cv::Mat>(videoStreamHeight, videoStreamWidth, CV_8UC3);
            uint8_t* dstData[1] = { img->data };
            const int dstLinesize[1] = { static_cast<int>(img->step) };
            sws_scale(pSwsContext, pAVFrame->data, pAVFrame->linesize, 0, pAVFrame->height,
                      dstData, dstLinesize);

            // 真正解出一帧 → 刷新看门狗时间戳（关键：仅在解出视频帧时刷新）。
            lastFrameTime = std::chrono::steady_clock::now();

#ifndef SHOW
            cv::imshow("Video_Stream", *img);
            cv::waitKey(1);
#endif
            latestFrame = img;

            // ── 节奏限流 ──
            const auto now = std::chrono::steady_clock::now();
            if (now - lastSubmit < kStreamSubmitInterval)
            {
                continue;
            }
            lastSubmit = now;

            // 以 shared_ptr 拷贝捕获，tryStart 返回 false 时也不丢帧。
            threadPool.tryStart([&, latestFrame]() {
                thread_local QRScanner qrScanners;
                std::string str;
                qrScanners.decodeSingle(*latestFrame, str);
                if (str.size() < 85)
                {
                    return;
                }
                // 崩坏3 B站服二维码固定标识为 "8F3"。
                if (std::string_view view(str.c_str() + 79, 3); view != "8F3")
                {
                    return;
                }
                const std::string& ticket = str.substr(str.length() - 24);
                if (lastTicket == ticket)
                {
                    return;
                }
                if (mtx.try_lock())
                {
                    if (!m_stop.load())
                    {
                        mtx.unlock();
                        return;
                    }
                    if (ret = scanCheck(ticket); ret == ScanRet::SUCCESS)
                    {
                        lastTicket = ticket;
                        nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                        if (config["auto_login"])
                        {
                            continueLastLogin();
                        }
                        else
                        {
                            Q_EMIT loginConfirm(GameType::Honkai3_BiliBili, false);
                        }
                    }
                    else
                    {
                        Q_EMIT loginResults(ret);
                    }
                    stop();
                    mtx.unlock();
                }
            });
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

/**
 * @brief 根据解码器原始分辨率，计算扫码用的目标宽高。
 *
 * 设计要点：
 *  - 竖屏流（宽 < 高）或不超过 720p 的流保持原样（已是 QR 友好档）；
 *  - 高于 720p 的统一缩放到 720p 高度，按源宽高比等比缩放宽度。
 * 这样既能降低 WeChatQRCode DNN 解码耗时，又保证二维码足够大可被识别，
 * 避免旧版「÷1.5」把 900p 弄成 600p 反而更难扫的缺陷。
 */
void QRCodeForStream::setStreamHW()
{
    const int srcW = pAVCodecContext->width;
    const int srcH = pAVCodecContext->height;
    if (srcW < srcH || srcH <= 720)
    {
        videoStreamWidth = srcW;
        videoStreamHeight = srcH;
    }
    else
    {
        videoStreamWidth = static_cast<int>(std::round(srcW * 720.0 / srcH));
        videoStreamHeight = 720;
    }
}

/**
 * @brief 请求停止扫码。
 *
 * 注意命名语义：本类用 m_stop == true 表示「正在运行」，false 表示「停止」。
 * 因此「停止」是 store(false)，与直觉相反，调用方需知悉。
 */
void QRCodeForStream::stop()
{
    m_stop.store(false);
}

/**
 * @brief 设置直播流地址与 FFmpeg 输入选项。
 *
 * @param url    直播流直链（如 B站 bilivideo CDN 的 .flv 地址）。
 * @param heard 额外的 HTTP 头键值对（当前调用方通常传空 map）。
 *
 * 说明：heard 中的每一项会被逐条 av_dict_set 到输入字典；其余为固定的
 * 低延迟 / 小缓冲选项，用于直播场景降低首帧延迟、避免长时间缓冲堆积。
 */
void QRCodeForStream::setUrl(const std::string& url, const std::map<std::string, std::string> heard)
{
    streamUrl = url;
    for (const auto& it : heard)
    {
        av_dict_set(&pAvdictionary, it.first.c_str(), it.second.c_str(), 0);
    }
    av_dict_set(&pAvdictionary, "max_delay", "0", 0);
    av_dict_set(&pAvdictionary, "probesize", "1024", 0);
    av_dict_set(&pAvdictionary, "packetsize", "128", 0);
    av_dict_set(&pAvdictionary, "rtbufsize", "0", 0);
    av_dict_set(&pAvdictionary, "delay", "0", 0);
    av_dict_set(&pAvdictionary, "buffer_size", "1000", 0);
}

/**
 * @brief 打开直播流并完成解码器 / 转换器初始化。
 *
 * @return true  初始化成功，可进入扫码循环。
 *         false 任意环节失败（已通过 streamError 信号给出中文提示）。
 *
 * 流程：打开输入 → 取流信息 → 定位视频流 → 打开解码器 → 计算目标分辨率
 *       → 创建图像转换上下文 → 分配包 / 帧。任一环节失败立即返回 false。
 */
auto QRCodeForStream::init() -> bool
{
    if (avformat_open_input(&pAVFormatContext, streamUrl.c_str(), NULL, &pAvdictionary) != 0)
    {
        std::string error_msg = "无法打开直播流。请检查：\n1. 直播间ID是否正确\n2. 直播是否正在进行\n3. 网络连接是否正常";
        std::cerr << "[FFmpeg] " << error_msg << std::endl;
        emit streamError(QString::fromStdString(error_msg));
        return false;
    }
    if (avformat_find_stream_info(pAVFormatContext, NULL) < 0)
    {
        std::string error_msg = "无法获取流信息。直播流可能已中断或格式不支持";
        std::cerr << "[FFmpeg] " << error_msg << std::endl;
        emit streamError(QString::fromStdString(error_msg));
        return false;
    }
    AVStream* videoStream = nullptr;
    for (int i = 0; i < pAVFormatContext->nb_streams; i++)
    {
        if (pAVFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStream = pAVFormatContext->streams[i];
            break;
        }
    }
    if (videoStream == nullptr)
    {
        std::string error_msg = "直播流中未找到视频流。可能是纯音频直播";
        std::cerr << "[FFmpeg] " << error_msg << std::endl;
        emit streamError(QString::fromStdString(error_msg));
        return false;
    }
    videoStreamIndex = videoStream->index;
    const AVCodec* decoder{ avcodec_find_decoder(videoStream->codecpar->codec_id) };
    if (decoder == nullptr)
    {
        std::string error_msg = "未找到视频解码器。视频编码格式可能不支持";
        std::cerr << "[FFmpeg] " << error_msg << std::endl;
        emit streamError(QString::fromStdString(error_msg));
        return false;
    }
    pAVCodecContext = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(pAVCodecContext, videoStream->codecpar);
    if (avcodec_open2(pAVCodecContext, decoder, NULL) < 0)
    {
        std::string error_msg = "无法打开视频解码器。可能是解码器初始化失败";
        std::cerr << "[FFmpeg] " << error_msg << std::endl;
        emit streamError(QString::fromStdString(error_msg));
        return false;
    }
    setStreamHW();
    pSwsContext = sws_getContext(
        pAVCodecContext->width, pAVCodecContext->height, pAVCodecContext->pix_fmt,
        videoStreamWidth, videoStreamHeight, AV_PIX_FMT_BGR24, SWS_BILINEAR, NULL, NULL, NULL);
    if (pSwsContext == nullptr)
    {
        std::string error_msg = "无法初始化图像转换器。可能是内存不足";
        std::cerr << "[FFmpeg] " << error_msg << std::endl;
        emit streamError(QString::fromStdString(error_msg));
        return false;
    }
    pAVPacket = av_packet_alloc();
    pAVFrame = av_frame_alloc();
    std::cerr << "[FFmpeg] 直播流初始化成功，分辨率: " 
              << videoStreamWidth << "x" << videoStreamHeight << std::endl;
    return true;
}

/**
 * @brief 在已拿到 ticket 的前提下，完成「确认登录」流程。
 *
 * 根据服务器类型（官服 / 崩坏3 B站服）调用对应的确认接口，
 * 并通过 loginResults 信号回报最终结果（SUCCESS / FAILURE_2）。
 */
void QRCodeForStream::continueLastLogin()
{
    switch (servertype)
    {
        using enum ServerType;
    case Official:
    {
        bool b = ConfirmQRLogin(confirmUrl, uid, gameToken, lastTicket, gameType);
        if (b)
        {
            Q_EMIT loginResults(ScanRet::SUCCESS);
        }
        else
        {
            Q_EMIT loginResults(ScanRet::FAILURE_2);
        }
    }
    break;
    case BH3_BiliBili:
    {
        ret = scanConfirm(lastTicket, uid, gameToken, m_name);
        Q_EMIT loginResults(ret);
    }
    break;
    default:
        break;
    }
}

/**
 * @brief 解码子线程入口（QThread::run 重写）。
 *
 * 流程：设置线程池并发数 → 置运行标志 → init() 打开直播流 →
 * 按服务器类型进入对应扫码主循环（LoginOfficial / LoginBH3BiliBili）→
 * 结束时释放所有 FFmpeg 资源并复位指针。
 * init() 失败时已在内部发射 streamError，此处不再重复发射。
 */
void QRCodeForStream::run()
{
    threadPool.setMaxThreadCount(threadNumber);
    m_stop.store(true);
    ret = ScanRet::UNKNOW;
    //TODO 获取直播流地址放在这里
    if (init())
    {
#ifndef SHOW
        cv::namedWindow("Video_Stream", cv::WINDOW_AUTOSIZE);
        cv::resizeWindow("Video_Stream", videoStreamWidth / 2, videoStreamHeight / 2);
#endif
        switch (servertype)
        {
            using enum ServerType;
        case Official:
            LoginOfficial();
            break;
        case BH3_BiliBili:
            LoginBH3BiliBili();
            break;
        default:
            break;
        }
    }
    // 注意：init()失败时已经在init()内部发射了streamError信号
    // 这里不需要再发射loginResults信号
    if (ret == ScanRet::LIVESTOP)
    {
        emit loginResults(ret);
    }
#ifndef SHOW
    cv::destroyWindow("Video_Stream");
#endif
    avformat_close_input(&pAVFormatContext);
    avcodec_free_context(&pAVCodecContext);
    sws_freeContext(pSwsContext);
    av_dict_free(&pAvdictionary);
    av_frame_free(&pAVFrame);
    av_packet_free(&pAVPacket);
    pAVFormatContext = nullptr;
    pAVCodecContext = nullptr;
    pSwsContext = nullptr;
    pAvdictionary = nullptr;
    pAVFrame = nullptr;
    pAVPacket = nullptr;
}