#include "Network.hpp"
#include <algorithm>

int Network::addDevice(std::unique_ptr<Device> dev) 
{
    int id = dev->id();
    devices_.push_back(std::move(dev));
    return id;
}

int Network::addLink(int a, int b, double bandwidthMbps, double latencyMs) 
{
    Link link;
    link.id            = nextLinkId_++;
    link.nodeA         = a;
    link.nodeB         = b;
    link.bandwidthMbps = bandwidthMbps;
    link.latencyMs     = latencyMs;
    link.currentLoad   = 0.0;

    links_.push_back(link);
    return link.id;
}

Device* Network::getDevice(int id) 
{
    for (auto& dev : devices_) {
        if (dev->id() == id) return dev.get();
    }
    return nullptr;
}

const Device* Network::getDevice(int id) const 
{
    for (const auto& dev : devices_) {
        if (dev->id() == id) return dev.get();
    }
    return nullptr;
}

const Link* Network::findLink(int a, int b) const 
{
    for (const auto& link : links_) {
        if ((link.nodeA == a && link.nodeB == b) ||
            (link.nodeA == b && link.nodeB == a)) {
            return &link;
        }
    }
    return nullptr;
}

void Network::spawnPacketOnLink(const Packet& pkt, int fromNode, int toNode) 
{
    const Link* link = findLink(fromNode, toNode);
    if (!link) return;

    InFlightPacket f;
    f.pkt      = pkt;
    f.linkId   = link->id;
    f.fromNode = fromNode;
    f.toNode   = toNode;
    f.t        = 0.0;

    double latencySec = link->latencyMs / 1000.0;
    double bits       = static_cast<double>(pkt.sizeBytes) * 8.0;
    double bwbps      = link->bandwidthMbps * 1'000'000.0;
    double serTime    = bits / bwbps;

    double physicalTime = latencySec + serTime;

    // visual hack
    f.travelTime = physicalTime * 50.0;     // exaggerate
    if (f.travelTime < 0.5) f.travelTime = 0.5;

    inFlight_.push_back(f);
}

void Network::updatePackets(double dt) 
{
    for (auto it = inFlight_.begin(); it != inFlight_.end();) {
        it->t += dt / it->travelTime;

        if (it->t >= 1.0) {
            Device* dst = getDevice(it->toNode);
            if (dst) dst->onPacketReceived(it->pkt);
            it = inFlight_.erase(it);
        } else {
            ++it;
        }
    }
}
