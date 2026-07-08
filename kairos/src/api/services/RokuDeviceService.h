#pragma once
#include "../IKairosService.h"
#include "../ServiceContext.h"

class RokuDeviceService final : public IKairosService {
public:
    explicit RokuDeviceService(const ServiceContext& ctx);
    void registerRoutes(httplib::Server& svr) override;

private:
    Database& db_;
};
