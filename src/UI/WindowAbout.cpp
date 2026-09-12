/**
 * @file WindowAbout.cpp
 * @brief 「关于」对话框：展示版本与 Qt 版本信息。
 */

#include "WindowAbout.h"

WindowAbout::WindowAbout(QWidget* parent) :
    QDialog(parent)
{
    setWindowTitle("关于 MHY_Scanner");
    setFixedSize(420, 220);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("MHY_Scanner"), this);
    title->setStyleSheet("font-size: 20px; font-weight: 700; color: #1E293B;");
    title->setAlignment(Qt::AlignCenter);

    auto* body = new QLabel(
        QStringLiteral("版本 %1\nQt Version: %2\n\n米哈游游戏登录二维码辅助工具")
            .arg(QStringLiteral(MHY_Scanner_VERSION))
            .arg(QStringLiteral(QT_VERSION_STR)),
        this);
    body->setStyleSheet("font-size: 13px; color: #475569;");
    body->setAlignment(Qt::AlignCenter);
    body->setWordWrap(true);

    layout->addStretch();
    layout->addWidget(title);
    layout->addWidget(body);
    layout->addStretch();
    setLayout(layout);
}
