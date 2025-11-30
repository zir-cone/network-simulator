#pragma once
#include "Device.hpp"
#include <memory>
#include <vector>

struct Link 
{
    int id;
    int nodeA;
    int nodeB;
    double bandwidthMbps;
    double latencyMs;
    double currentLoad = 0.0;
};

struct InFlightPacket
{
    Packet pkt;
    int    linkId;
    int    fromNode;
    int    toNode;
    double t          = 0.0; // 0.0 @ fromNode; 1.0 @ toNode
    double travelTime = 0;   // seconds to go from A to B on this link
};

class Network 
{
public:
    int addDevice(std::unique_ptr<Device> dev);
    int addLink(int a, int b, double bandwidthMbps, double latencyMs);

    Device* getDevice(int id);
    const Device* getDevice(int id) const;

    const std::vector<std::unique_ptr<Device>>& devices() const { return devices_; }
    std::vector<std::unique_ptr<Device>>& devices() { return devices_; }

    const std::vector<Link>& links() const { return links_; }
    std::vector<Link>& links() { return links_; }

    void spawnPacketOnLink(const Packet& pkt, int fromNode, int toNode);
    void updatePackets(double dt);
    const std::vector<InFlightPacket>& inFlightPackets() const { return inFlight_; }

private:
    const Link* findLink(int a, int b) const;

    std::vector<std::unique_ptr<Device>> devices_;
    std::vector<Link> links_;
    std::vector<InFlightPacket> inFlight_;
    int nextLinkId_ = 0;
};
