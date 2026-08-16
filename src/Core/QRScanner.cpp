#include "QRScanner.h"

#include <windows.h>
#include <filesystem>

// 模型路径改为相对 exe 所在目录解析，避免工作目录不同导致加载失败（表现为「无反应」）。
static std::filesystem::path getModelDir()
{
    WCHAR exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path() / "ScanModel";
}

QRScanner::QRScanner()
{
    const auto modelDir = getModelDir();
    detector = cv::makePtr<cv::wechat_qrcode::WeChatQRCode>(
        (modelDir / "detect.prototxt").string(),
        (modelDir / "detect.caffemodel").string(),
        (modelDir / "sr.prototxt").string(),
        (modelDir / "sr.caffemodel").string());
    detector->setScaleFactor(0.4);
}

QRScanner::~QRScanner()
{
}

void QRScanner::decodeSingle(const cv::Mat& img, std::string& qrCode)
{
#ifndef TESTSPEED
    auto startTime = std::chrono::high_resolution_clock::now();
#endif
    const std::vector<std::string>& strDecoded = detector->detectAndDecode(img);
    if (strDecoded.size() > 0)
    {
        qrCode = strDecoded[0];
    }
#ifndef TESTSPEED
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
    std::cout << static_cast<float>(duration) / 1000000 << " decode: " << qrCode << std::endl;
#endif
}

void QRScanner::decodeMultiple(const cv::Mat& img, std::string& qrCode)
{
    const std::vector<std::string>& strDecoded = detector->detectAndDecode(img);
    for (int i = 0; i < strDecoded.size(); i++)
    {
        qrCode = strDecoded[i];
#ifdef _DEBUG
        std::cout << "decode:" << qrCode << std::endl;
#endif // DEBUG
    }
}