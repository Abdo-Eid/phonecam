#pragma once
#include <functional>
#include <memory>
#include "connection/ConnectionManager.h"
namespace phonecam::tray {
class QrPairDialog {
public:
    QrPairDialog(phonecam::connection::ConnectionManager& connection, std::function<void()> manual,
                 std::function<void()> onPaired = {});
    ~QrPairDialog();
    void Show();
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};
}
