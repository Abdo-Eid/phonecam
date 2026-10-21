#pragma once
#include <memory>
#include "connection/ConnectionManager.h"

namespace phonecam::tray {
class WifiDialog {
public:
    explicit WifiDialog(phonecam::connection::ConnectionManager& connection);
    ~WifiDialog();
    void Show();
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};
}
