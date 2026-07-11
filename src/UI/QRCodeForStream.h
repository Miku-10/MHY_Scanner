#pragma once

// ── 标准库 ──
#include <atomic>
#include <memory>
#include <string_view>
#include <map>

// ── FFmpeg（C 接口，需用 extern "C" 包裹）──
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
};

// ── Qt ──
#include <QThread>
#include <QMutex>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#include <QThreadPool>

// ── 项目头文件 ──
#include "ApiDefs.hpp"
#include "ConfigDate.h"
#include "ScannerBase.hpp"

/**
 * @brief 直播流扫码核心类（继承自 QThread，在子线程中解码直播画面并识别二维码）。
 *
 * 职责：
 *  - 通过 FFmpeg 拉取 B站 / 抖音直播流并解码视频帧；
 *  - 用「最新帧 + 固定节奏」机制把视频帧提交给 QR 解码线程池，
 *    彻底解决旧版「逐帧 tryStart 丢帧 → 大概率无反应」的问题；
 *  - 识别米游社登录二维码后，回调对应服务器完成登录流程。
 *
 * 典型调用顺序：setLoginInfo → setServerType → setUrl → start()（触发 run()）。
 */
class QRCodeForStream final :
    public QThread,
    public ScannerBase
{
    Q_OBJECT
public:
    QRCodeForStream(QObject* parent = nullptr);
    ~QRCodeForStream();
    Q_DISABLE_COPY_MOVE(QRCodeForStream)

    void setLoginInfo(const std::string_view uid, const std::string_view gameToken);
    void setLoginInfo(const std::string_view uid, const std::string_view gameToken, const std::string& name);
    void setServerType(const ServerType servertype);
    void setUrl(const std::string& url, const std::map<std::string, std::string> heard = {});
    auto init() -> bool;
    void run();
    void stop();
    void continueLastLogin();

Q_SIGNALS:
    void loginResults(const ScanRet ret);
    void loginConfirm(const GameType gameType, bool b);
    void streamError(const QString& errorMessage);

private:
    std::mutex mtx;                  // 登录请求临界区锁，避免多线程重复提交登录
    void LoginOfficial();            // 官服直播流扫码主循环
    void LoginBH3BiliBili();         // 崩坏3 B站服直播流扫码主循环
    void setStreamHW();              // 依据源分辨率计算扫码目标宽高

    std::string streamUrl{};         // 直播流直链
    std::string m_name;              // 崩坏3 等需要昵称的服务器用
    ConfigDate* m_config;            // 全局配置单例（读取 auto_login 等）
    ServerType servertype;           // 当前服务器类型，决定走哪个扫码主循环
    ScanRet ret = ScanRet::UNKNOW;   // 扫码结果状态，最终通过 loginResults 上报
    AVDictionary* pAvdictionary;      // FFmpeg 输入选项字典
    AVFormatContext* pAVFormatContext;
    AVCodecContext* pAVCodecContext;
    SwsContext* pSwsContext;         // 图像格式转换上下文（原始帧 → BGR）
    AVFrame* pAVFrame;               // 解码后的原始帧
    AVPacket* pAVPacket;             // 压缩数据包
    int videoStreamIndex{ 0 };       // 视频流在容器中的索引
    int videoStreamWidth{};          // 扫码用目标宽度
    int videoStreamHeight{};         // 扫码用目标高度
    const int threadNumber{ 3 };     // 解码线程池并发数（兼顾吞吐与丢帧率）
    QThreadPool threadPool;          // QR 解码线程池
    std::atomic<bool> m_stop;        // 运行标志：true=运行，false=停止（语义反直觉，见 stop()）
};