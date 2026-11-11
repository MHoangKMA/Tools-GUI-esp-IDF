#ifndef _DNS_SERVER_H_
#define _DNS_SERVER_H_

// Captive-portal DNS: answers every A query with the configuration AP's address so that a
// phone that joins the setup hotspot is sent to the configuration page.
// NOTE: re-written from the interface used by wifi_configuration_ap.cc (the original file was
// not available), not byte-identical to upstream esp-wifi-connect.

#include <atomic>

#include <esp_netif_ip_addr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class DnsServer {
public:
    DnsServer();
    ~DnsServer();

    DnsServer(const DnsServer&) = delete;
    DnsServer& operator=(const DnsServer&) = delete;

    void Start(esp_ip4_addr_t gateway);
    void Stop();

private:
    static void TaskEntry(void* arg);
    void Run();

    esp_ip4_addr_t gateway_ = {};
    int socket_ = -1;
    std::atomic<bool> running_{false};
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t done_ = nullptr;
};

#endif  // _DNS_SERVER_H_
