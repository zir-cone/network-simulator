#pragma once
#include <cstdint>
#include <string>

enum class NetworkScope 
{
    Local,
    Enterprise,
    Global
};

enum class TransportProtocol {
    TCP,
    UDP
};

enum class ApplicationProtocol
{
    HTTPS,
    HTTP,
    DNS,
    OTHER
};

struct Packet {
    std::uint64_t id;
    int srcNodeId;
    int dstNodeId;
    std::size_t sizeBytes;
    double createdAt;
    std::string srcIp;
    std::string dstIp;
    std::uint16_t srcPort = 0;
    std::uint16_t dstPort = 0;
    TransportProtocol transport = TransportProtocol::TCP;
    ApplicationProtocol app = ApplicationProtocol::OTHER;
};


class Device 
{
public:
    explicit Device(int id, NetworkScope scope)
        : id_(id), scope_(scope) {}

    virtual ~Device() = default;

    int id() const { return id_; }
    NetworkScope scope() const { return scope_; }

    virtual void tick(double now) = 0; // called on every sim step
    virtual void onPacketReceived(const Packet& pkt) = 0; // called on packet arrival

protected:
    int id_;
    NetworkScope scope_;
};
