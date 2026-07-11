#include "QRCodeForStream.h"

#include <chrono>
#include <cmath>
#include <string>
#include <string_view>

#include "QRScanner.h"
#include "MhyApi.hpp"

// 直播流扫码的提交节奏（毫秒）。
// 直播流帧率高（30~60fps），若对每一帧都调用 threadPool.tryStart 提交 QR 解码，
// 2~3 个线程的线程池会被慢速 WeChatQRCode DNN 解码占满，tryStart 在无空闲线程时
// 静默返回 false 丢帧（含二维码帧），表现为“大概率无反应、偶尔能扫上”。
// 这里用“最新帧”机制 + 固定节奏解决：解码循环对每一帧都执行 sws_scale，但只在
// 节奏窗口打开时把【当前最新一帧】提交给线程池；窗口关闭期间的帧只从解码器排空、
// 并把最新一帧缓存下来，绝不直接丢弃。这样既保证二维码帧一定会被扫到，
// 又不会因提交过密压垮线程池。节奏与屏幕扫码路径（QRCodeForScreen 的 DELAYED=200）一致。
static constexpr auto kStreamSubmitInterval = std::chrono::milliseconds(200);
// 流卡死看门狗：超过此时长未读到任何一帧，判定直播流已中断并给出反馈。
static constexpr auto kStreamStallTimeout = std::chrono::seconds(10);

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
    av_log_set_level(AV_LOG_FATAL);
    m_config = &(ConfigDate::getInstance());
}

QRCodeForStream::~QRCodeForStream()
{
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

void QRCodeForStream::LoginOfficial()
{
    // 最新帧：解码循环每解出一帧都更新它；节奏窗口打开时只提交最新一帧，
    // 因此即便一个窗口内出现多帧含二维码，也只会提交（最新）一帧，绝不丢码。
    std::shared_ptr<cv::Mat> latestFrame;
    auto lastSubmit = std::chrono::steady_clock::now() - kStreamSubmitInterval;
    auto lastFrameTime = std::chrono::steady_clock::now();
    while (m_stop.load())
    {
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        lastFrameTime = std::chrono::steady_clock::now();
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
#ifndef SHOW
            cv::imshow("Video_Stream", *img);
            cv::waitKey(1);
#endif
            // 始终缓存最新一帧；窗口关闭期间的帧不会真正丢失。
            latestFrame = img;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastSubmit < kStreamSubmitInterval)
            {
                continue;
            }
            lastSubmit = now;
            threadPool.tryStart([&, frame = std::move(latestFrame)]() {
                thread_local QRScanner qrScanners;
                std::string str;
                qrScanners.decodeSingle(*frame, str);
                if (str.size() < 85)
                {
                    return;
                }
                std::string_view view(str.c_str() + 79, 3);
                if (!setGameType.contains(view))
                {
                    return;
                }
                const std::string_view ticket(str.data() + str.size() - 24, 24);
                setGameType[view]();
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
        // 流卡死看门狗：长时间读不到帧，说明直播流已中断。
        if (std::chrono::steady_clock::now() - lastFrameTime > kStreamStallTimeout)
        {
            std::string error_msg = "直播流已中断或无画面数据。请检查直播是否仍在进行、网络是否稳定";
            std::cerr << "[FFmpeg] " << error_msg << std::endl;
            emit streamError(QString::fromStdString(error_msg));
            ret = ScanRet::LIVESTOP;
            break;
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::LoginBH3BiliBili()
{
    // 最新帧：解码循环每解出一帧都更新它；节奏窗口打开时只提交最新一帧，
    // 因此即便一个窗口内出现多帧含二维码，也只会提交（最新）一帧，绝不丢码。
    std::shared_ptr<cv::Mat> latestFrame;
    auto lastSubmit = std::chrono::steady_clock::now() - kStreamSubmitInterval;
    auto lastFrameTime = std::chrono::steady_clock::now();
    while (m_stop.load())
    {
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        lastFrameTime = std::chrono::steady_clock::now();
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
#ifndef SHOW
            cv::imshow("Video_Stream", *img);
            cv::waitKey(1);
#endif
            // 始终缓存最新一帧；窗口关闭期间的帧不会真正丢失。
            latestFrame = img;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastSubmit < kStreamSubmitInterval)
            {
                continue;
            }
            lastSubmit = now;
            threadPool.tryStart([&, frame = std::move(latestFrame)]() {
                thread_local QRScanner qrScanners;
                std::string str;
                qrScanners.decodeSingle(*frame, str);
                if (str.size() < 85)
                {
                    return;
                }
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
        // 流卡死看门狗：长时间读不到帧，说明直播流已中断。
        if (std::chrono::steady_clock::now() - lastFrameTime > kStreamStallTimeout)
        {
            std::string error_msg = "直播流已中断或无画面数据。请检查直播是否仍在进行、网络是否稳定";
            std::cerr << "[FFmpeg] " << error_msg << std::endl;
            emit streamError(QString::fromStdString(error_msg));
            ret = ScanRet::LIVESTOP;
            break;
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::setStreamHW()
{
    // 分辨率归一化：竖屏流或常见 480p/720p 保持原样（已是 QR 友好档）；
    // 高于 720p 的统一缩放到 720p 高度，既降低 WeChatQRCode DNN 解码耗时、
    // 又保证二维码足够大可被识别（避免原 ÷1.5 把 900p 弄成 600p 反而更难扫）。
    // 缩放保持源宽高比。
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

void QRCodeForStream::stop()
{
    m_stop.store(false);
}

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