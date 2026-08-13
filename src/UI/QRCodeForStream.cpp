#include "QRCodeForStream.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "QRScanner.h"
#include "MhyApi.hpp"

// 直播流扫码的提交节奏（毫秒）。
// 直播流帧率高（30~60fps），若对每一帧都调用 threadPool.tryStart 提交 QR 解码，
// 2~3 个线程的线程池会被慢速 WeChatQRCode DNN 解码占满，tryStart 在无空闲线程时
// 静默返回 false 丢帧（含二维码帧），表现为「大概率无反应、偶尔能扫上」。
// 这里用「最新帧」机制 + 固定节奏解决：解码循环对每一帧都执行 sws_scale，但只在
// 节奏窗口打开时把【当前最新一帧】提交给线程池；窗口关闭期间的帧只从解码器排空、
// 并把最新一帧缓存下来，绝不直接丢弃。这样既保证二维码帧一定会被扫到，
// 又不会因提交过密压垮线程池。节奏与屏幕扫码路径保持一致（200ms）。
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

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view stoken)
{
    this->uid = uid;
    this->stoken = stoken;
}

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view stoken, const std::string& name)
{
    this->uid = uid;
    this->stoken = stoken;
    this->m_name = name;
}

void QRCodeForStream::setMid(const std::string& mid)
{
    this->mid = mid;
}

void QRCodeForStream::setServerType(const ServerType servertype)
{
    this->servertype = servertype;
}

void QRCodeForStream::LoginOfficial()
{
    // 看门狗基准：记录起始时刻，循环中每成功读到一帧就刷新 lastFrameTime。
    lastFrameTime = std::chrono::steady_clock::now();
    while (m_stop.load())
    {
        // 无画面看门狗：超过 kStreamStallTimeout 未读到任何一帧，判定直播流已中断。
        auto now = std::chrono::steady_clock::now();
        if (lastFrameTime.time_since_epoch().count() != 0 &&
            now - lastFrameTime > kStreamStallTimeout)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        lastFrameTime = std::chrono::steady_clock::now();  // 成功读到包，刷新看门狗
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
            cv::Mat img(videoStreamHeight, videoStreamWidth, CV_8UC3);
            uint8_t* dstData[1] = { img.data };
            const int dstLinesize[1] = { static_cast<int>(img.step) };
            sws_scale(pSwsContext, pAVFrame->data, pAVFrame->linesize, 0, pAVFrame->height,
                      dstData, dstLinesize);
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            // ── 最新帧 + 节奏限流：根治「逐帧 tryStart 静默丢帧 → 大概率无反应」──
            // 每解出一帧都刷新 latestFrame（绝不丢弃）；仅在距上次提交 >= kStreamSubmitInterval
            // 且线程池有空位时，把【当前最新一帧】提交解码。这样二维码帧一定会被扫到，
            // 又不会因提交过密压垮线程池。
            latestFrame = std::make_shared<cv::Mat>(std::move(img));
            auto t = std::chrono::steady_clock::now();
            if (t - lastSubmitTime >= kStreamSubmitInterval &&
                threadPool.activeThreadCount() < threadNumber)
            {
                lastSubmitTime = t;
                auto frame = std::move(latestFrame);
                threadPool.start([this, frame]() {
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
                        passportQRUrl = PandaScanQRCode(scanUrl.data(), ticket, gameType);
                        if (!passportQRUrl.empty() && PassportQRLogin(passportQRUrl, stoken, mid, false))
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
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::LoginBH3BiliBili()
{
    // 看门狗基准：记录起始时刻，循环中每成功读到一帧就刷新 lastFrameTime。
    lastFrameTime = std::chrono::steady_clock::now();
    while (m_stop.load())
    {
        // 无画面看门狗：超过 kStreamStallTimeout 未读到任何一帧，判定直播流已中断。
        auto now = std::chrono::steady_clock::now();
        if (lastFrameTime.time_since_epoch().count() != 0 &&
            now - lastFrameTime > kStreamStallTimeout)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        lastFrameTime = std::chrono::steady_clock::now();  // 成功读到包，刷新看门狗
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
            cv::Mat img(videoStreamHeight, videoStreamWidth, CV_8UC3);
            uint8_t* dstData[1] = { img.data };
            const int dstLinesize[1] = { static_cast<int>(img.step) };
            sws_scale(pSwsContext, pAVFrame->data, pAVFrame->linesize, 0, pAVFrame->height,
                      dstData, dstLinesize);
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            // ── 最新帧 + 节奏限流：根治「逐帧 tryStart 静默丢帧 → 大概率无反应」──
            // 每解出一帧都刷新 latestFrame（绝不丢弃）；仅在距上次提交 >= kStreamSubmitInterval
            // 且线程池有空位时，把【当前最新一帧】提交解码。这样二维码帧一定会被扫到，
            // 又不会因提交过密压垮线程池。
            latestFrame = std::make_shared<cv::Mat>(std::move(img));
            auto t = std::chrono::steady_clock::now();
            if (t - lastSubmitTime >= kStreamSubmitInterval &&
                threadPool.activeThreadCount() < threadNumber)
            {
                lastSubmitTime = t;
                auto frame = std::move(latestFrame);
                threadPool.start([this, frame]() {
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
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::setStreamHW()
{
    if (pAVCodecContext->width < pAVCodecContext->height ||
        pAVCodecContext->height == 480 ||
        pAVCodecContext->height == 720)
    {
        videoStreamWidth = pAVCodecContext->width;
        videoStreamHeight = pAVCodecContext->height;
    }
    else
    {
        videoStreamWidth = pAVCodecContext->width / 1.5;
        videoStreamHeight = pAVCodecContext->height / 1.5;
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
    pAVFormatContext = avformat_alloc_context();
    if (avformat_open_input(&pAVFormatContext, streamUrl.c_str(), NULL, &pAvdictionary) != 0)
    {
        std::cerr << "Error opening input file" << std::endl;
        return false;
    }
    if (avformat_find_stream_info(pAVFormatContext, NULL) < 0)
    {
        std::cerr << "Error finding stream information" << std::endl;
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
        std::cerr << "No video stream found" << std::endl;
        return false;
    }
    videoStreamIndex = videoStream->index;
    const AVCodec* decoder{ avcodec_find_decoder(videoStream->codecpar->codec_id) };
    if (decoder == nullptr)
    {
        std::cerr << "Codec not found" << std::endl;
        return false;
    }
    pAVCodecContext = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(pAVCodecContext, videoStream->codecpar);
    if (avcodec_open2(pAVCodecContext, decoder, NULL) < 0)
    {
        std::cerr << "Error opening codec" << std::endl;
        return false;
    }
    setStreamHW();
    pSwsContext = sws_getContext(
        pAVCodecContext->width, pAVCodecContext->height, pAVCodecContext->pix_fmt,
        videoStreamWidth, videoStreamHeight, AV_PIX_FMT_BGR24, SWS_BILINEAR, NULL, NULL, NULL);
    pAVPacket = av_packet_alloc();
    pAVFrame = av_frame_alloc();
    return true;
}

void QRCodeForStream::continueLastLogin()
{
    switch (servertype)
    {
        using enum ServerType;
    case Official:
    {
        bool b = PassportQRLogin(passportQRUrl, stoken, mid, true);
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
        ret = scanConfirm(lastTicket, uid, stoken, m_name);
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