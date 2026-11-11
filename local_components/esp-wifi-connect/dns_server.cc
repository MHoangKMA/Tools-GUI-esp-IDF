#include "dns_server.h"

#include <cstring>

#include <esp_log.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#define TAG "DnsServer"

namespace {
constexpr uint16_t kDnsPort = 53;
constexpr size_t kMaxPacket = 512;
constexpr uint16_t kTypeA = 1;
constexpr uint16_t kClassIn = 1;
}  // namespace

DnsServer::DnsServer() { done_ = xSemaphoreCreateBinary(); }

DnsServer::~DnsServer() {
    Stop();
    if (done_ != nullptr) {
        vSemaphoreDelete(done_);
    }
}

void DnsServer::Start(esp_ip4_addr_t gateway) {
    if (running_.load()) {
        return;
    }
    gateway_ = gateway;

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ < 0) {
        ESP_LOGE(TAG, "Cannot create socket");
        return;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(kDnsPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ESP_LOGE(TAG, "Cannot bind port %d", kDnsPort);
        close(socket_);
        socket_ = -1;
        return;
    }
    timeval timeout = {.tv_sec = 1, .tv_usec = 0};  // lets Stop() end the loop within a second
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    running_ = true;
    xSemaphoreTake(done_, 0);
    if (xTaskCreate(TaskEntry, "dns_server", 4096, this, 5, &task_) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start DNS task");
        running_ = false;
        close(socket_);
        socket_ = -1;
    }
}

void DnsServer::Stop() {
    if (!running_.load()) {
        return;
    }
    running_ = false;
    xSemaphoreTake(done_, pdMS_TO_TICKS(3000));  // the task gives it when it leaves Run()
    task_ = nullptr;
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
}

void DnsServer::TaskEntry(void* arg) {
    auto* self = static_cast<DnsServer*>(arg);
    self->Run();
    xSemaphoreGive(self->done_);
    vTaskDelete(nullptr);
}

void DnsServer::Run() {
    uint8_t packet[kMaxPacket];
    while (running_.load()) {
        sockaddr_in client = {};
        socklen_t client_length = sizeof(client);
        int received = recvfrom(socket_, packet, sizeof(packet) - 16, 0,
                                reinterpret_cast<sockaddr*>(&client), &client_length);
        if (received < 12) {
            continue;  // timeout, error or runt packet
        }
        // Header: id(2) flags(2) qdcount(2) ancount(2) nscount(2) arcount(2)
        const uint16_t flags = (packet[2] << 8) | packet[3];
        const uint16_t question_count = (packet[4] << 8) | packet[5];
        if ((flags & 0x8000) != 0 || question_count < 1) {
            continue;  // a response or an empty query
        }

        // Skip the first question name
        int offset = 12;
        while (offset < received && packet[offset] != 0) {
            offset += packet[offset] + 1;
        }
        offset += 1;  // the terminating zero
        if (offset + 4 > received) {
            continue;
        }
        const uint16_t type = (packet[offset] << 8) | packet[offset + 1];
        const uint16_t qclass = (packet[offset + 2] << 8) | packet[offset + 3];
        const int question_end = offset + 4;

        const bool answer = (type == kTypeA && qclass == kClassIn);
        // Response: copy id + question, set QR + RA, NOERROR; one answer for A, none otherwise
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[4] = 0x00;
        packet[5] = 0x01;
        packet[6] = 0x00;
        packet[7] = answer ? 0x01 : 0x00;
        packet[8] = packet[9] = packet[10] = packet[11] = 0x00;
        int length = question_end;
        if (answer) {
            const uint8_t record[] = {0xC0, 0x0C,              // name: pointer to the question
                                      0x00, 0x01, 0x00, 0x01,  // type A, class IN
                                      0x00, 0x00, 0x00, 0x3C,  // TTL 60 s
                                      0x00, 0x04};             // RDLENGTH
            memcpy(packet + length, record, sizeof(record));
            length += sizeof(record);
            memcpy(packet + length, &gateway_.addr, 4);  // already in network byte order
            length += 4;
        }
        sendto(socket_, packet, length, 0, reinterpret_cast<sockaddr*>(&client), client_length);
    }
}
