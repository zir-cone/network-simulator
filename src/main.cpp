#include <SFML/Graphics.hpp>
#include <memory>
#include <iostream>
#include <random>
#include <cctype>
#include <cstdio>

#include "sim/Network.hpp"
#include "sim/Simulation.hpp"
#include "gui/Renderer.hpp"
#include "sim/Device.hpp"

// ------------------------
// Device types
// ------------------------

class HomeDevice : public Device
{
public:
    HomeDevice(int id, NetworkScope scope, std::string ip, std::string name)
        : Device(id, scope),
          ip_(std::move(ip)),
          name_(std::move(name))
    {
        // deduce rough type and user from name
        std::string lower = name_;
        for (char &c : lower) c = static_cast<char>(std::tolower(c));

        if (lower.find("desktop") != std::string::npos)
            type_ = "Desktop PC";
        else if (lower.find("laptop") != std::string::npos)
            type_ = "Laptop";
        else if (lower.find("phone") != std::string::npos)
            type_ = "Smartphone";
        else if (lower.find("television") != std::string::npos ||
                 lower.find("tv") != std::string::npos)
            type_ = "Smart TV";
        else if (lower.find("fridge") != std::string::npos)
            type_ = "Smart Fridge";
        else if (lower.find("tablet") != std::string::npos)
            type_ = "Tablet";
        else
            type_ = "Endpoint";

        if (lower.find("john") != std::string::npos)
            user_ = "John";
        else
            user_ = "Family";

        // simple deterministic MAC from id
        char buf[18];
        std::snprintf(buf, sizeof(buf), "02:00:00:00:%02X:%02X",
                      (id >> 8) & 0xFF, id & 0xFF);
        mac_ = buf;

        publicIp_ = "203.0.113.5"; // example public IPv4 addr
    }

    const std::string& ip()       const { return ip_; }
    const std::string& name()     const { return name_; }
    const std::string& type()     const { return type_; }
    const std::string& user()     const { return user_; }
    const std::string& mac()      const { return mac_; }
    const std::string& publicIp() const { return publicIp_; }

    void tick(double now) override
    {
        (void)now; // no internal behavior atm; traffic driven by main()
    }

    void onPacketReceived(const Packet& pkt) override
    {
        // Placeholder for debugging
        (void)pkt;
    }

    DeviceInfo info() const override
    {
        return DeviceInfo{
            name_,
            user_,
            type_,
            ip_,
            publicIp_,
            mac_
        };
    }

private:
    std::string ip_;
    std::string name_;
    std::string type_;
    std::string user_;
    std::string mac_;
    std::string publicIp_;
};

class RouterDevice : public Device
{
public:
    RouterDevice(int id, NetworkScope scope, std::string ip)
        : Device(id, scope), ip_(std::move(ip)) {}

    const std::string& ip() const { return ip_; }

    void tick(double /*now*/) override
    {
        // main() will inspect pending queues and emulate WAN side
    }

    void onPacketReceived(const Packet& pkt) override
    {
        // Packets arriving from LAN to router
        if (pkt.dstPort == 53 && pkt.app == ApplicationProtocol::DNS) {
            pendingDns_.push_back(pkt);
        } else if (pkt.dstPort == 443 && pkt.app == ApplicationProtocol::HTTPS) {
            pendingHttps_.push_back(pkt);
        }
    }

    DeviceInfo info() const override
    {
        return DeviceInfo{
            "home-router",
            "ISP",
            "Router",
            ip_,
            "203.0.113.1",
            "00:11:22:33:44:55"
        };
    }

    std::vector<Packet> pendingDns_;
    std::vector<Packet> pendingHttps_;

private:
    std::string ip_;
};

struct ScheduledPacket
{
    Packet pkt;
    int    fromNode;
    int    toNode;
    double sendAt;   // simulation time when to inject into LAN
};

// main

int main()
{
    const unsigned WIDTH  = 1280;
    const unsigned HEIGHT = 720;

    sf::RenderWindow window(
        sf::VideoMode(WIDTH, HEIGHT),
        "40NetworkSimulator v1.1"
    );
    window.setFramerateLimit(60);

    Network network;
    std::vector<ScheduledPacket> scheduledPackets;

    int nextId = 0;

    // router in the middle of the home
    int routerId = network.addDevice(
        std::make_unique<RouterDevice>(nextId++, NetworkScope::Local, "192.168.0.1")
    );

    // helper lambda to add a home device
    auto addHome = [&](const std::string& ip, const std::string& name) {
        int id = network.addDevice(
            std::make_unique<HomeDevice>(nextId++, NetworkScope::Local, ip, name)
        );
        // connect via wi-fi or ethernet
        double bw   = 100.0; // Mbps
        double lat  = 5.0;   // ms
        if (name.find("TV") != std::string::npos ||
            name.find("Desktop") != std::string::npos ||
            name.find("television") != std::string::npos) {
            bw  = 1000.0;
            lat = 1.0;
        }
        network.addLink(routerId, id, bw, lat);
        return id;
    };

    // Household devices
    int familyPcId     = addHome("192.168.0.10", "family-desktop");
    int laptopId       = addHome("192.168.0.11", "personal-laptop");
    int phoneId        = addHome("192.168.0.12", "johns-phone");
    int tabletId       = addHome("192.168.0.13", "family-tablet"); // unused
    (void)tabletId;
    int tvId           = addHome("192.168.0.14", "family-television");
    int smartFridgeId  = addHome("192.168.0.20", "smart-fridge");

    Simulation sim(network);
    Renderer   renderer(window, network);

    bool paused    = false;
    sf::Clock clock;
    double timeScale = 1.0; // 1 simulated second per real second

    std::cout << "Controls:\n"
              << "  Space: pause/resume\n"
              << "  Up:    speed up (x10)\n"
              << "  Down:  slow down (/10)\n"
              << "  Esc:   quit\n";

    // trfc generation timers
    double simTime = 0.0;
    double nextDnsQueryTime      = 0.0;
    double nextWebBurstTime      = 0.0;
    double nextVideoChunkTime    = 0.0;
    double nextFridgePingTime    = 0.0;

    std::uint64_t nextPacketId = 1;

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> webClientDist(0, 2); // PC/laptop/phone

    auto deviceIp = [&](int id) -> std::string {
        if (auto* dev = dynamic_cast<HomeDevice*>(network.getDevice(id)))
            return dev->ip();
        if (auto* r = dynamic_cast<RouterDevice*>(network.getDevice(id)))
            return r->ip();
        return "0.0.0.0";
    };

    auto sendLanPacket = [&](int srcId, int dstId,
                             const std::string& srcIp,
                             const std::string& dstIp,
                             std::uint16_t srcPort,
                             std::uint16_t dstPort,
                             TransportProtocol transport,
                             ApplicationProtocol appProto,
                             std::size_t sizeBytes) {
        Packet p;
        p.id        = nextPacketId++;
        p.srcNodeId = srcId;
        p.dstNodeId = dstId;
        p.sizeBytes = sizeBytes;
        p.createdAt = simTime;
        p.srcIp     = srcIp;
        p.dstIp     = dstIp;
        p.srcPort   = srcPort;
        p.dstPort   = dstPort;
        p.transport = transport;
        p.app       = appProto;

        network.spawnPacketOnLink(p, srcId, dstId);
    };

    // main loop
    while (window.isOpen()) {
        sf::Event event{};
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) {
                window.close();
            }
            if (event.type == sf::Event::KeyPressed) {
                if (event.key.code == sf::Keyboard::Escape) {
                    window.close();
                } else if (event.key.code == sf::Keyboard::Space) {
                    paused = !paused;
                    std::cout << (paused ? "Paused\n" : "Resumed\n");
                } else if (event.key.code == sf::Keyboard::Up) {
                    if (timeScale < 10.0) timeScale *= 10.0;
                    std::cout << "Time scale: " << timeScale << "x\n";
                } else if (event.key.code == sf::Keyboard::Down) {
                    if (timeScale > 0.001) timeScale /= 10.0;
                    std::cout << "Time scale: " << timeScale << "x\n";
                }
            }
        }

        double dtReal = clock.restart().asSeconds();
        if (dtReal > 0.1) dtReal = 0.1;
        double dtSim = dtReal * timeScale;

        if (!paused) {
            sim.step(dtSim);
            simTime += dtSim;

            // LAN ---> router trfc

            // DNS queries every few seconds from a random client
            if (simTime >= nextDnsQueryTime) {
                int clientIdx = webClientDist(rng);
                int clientId = (clientIdx == 0 ? familyPcId
                               : clientIdx == 1 ? laptopId
                                                : phoneId);
                sendLanPacket(clientId, routerId,
                              deviceIp(clientId), deviceIp(routerId),
                              static_cast<std::uint16_t>(40000 + clientIdx), 53,
                              TransportProtocol::UDP,
                              ApplicationProtocol::DNS, 80);
                nextDnsQueryTime = simTime + 3.0;
            }

            // web browsing bursts (HTTPS)
            if (simTime >= nextWebBurstTime) {
                int clientIdx = webClientDist(rng);
                int clientId = (clientIdx == 0 ? familyPcId
                               : clientIdx == 1 ? laptopId
                                                : phoneId);

                for (int i = 0; i < 5; ++i) {
                    sendLanPacket(clientId, routerId,
                                  deviceIp(clientId), deviceIp(routerId),
                                  static_cast<std::uint16_t>(50000 + i), 443,
                                  TransportProtocol::TCP,
                                  ApplicationProtocol::HTTPS, 900);
                }

                nextWebBurstTime = simTime + 5.0;
            }

            // continuous video chunks from TV
            if (simTime >= nextVideoChunkTime) {
                sendLanPacket(tvId, routerId,
                              deviceIp(tvId), deviceIp(routerId),
                              60000, 443,
                              TransportProtocol::TCP,
                              ApplicationProtocol::HTTPS, 4000);

                nextVideoChunkTime = simTime + 0.4;
            }

            // smart fridge occasional telemetry
            if (simTime >= nextFridgePingTime) {
                sendLanPacket(smartFridgeId, routerId,
                              deviceIp(smartFridgeId), deviceIp(routerId),
                              55000, 443,
                              TransportProtocol::TCP,
                              ApplicationProtocol::HTTPS, 200);
                nextFridgePingTime = simTime + 10.0;
            }

            // router processes arrivals and schedules WAN responses

            auto* router = dynamic_cast<RouterDevice*>(network.getDevice(routerId));
            if (router) {
                // DNS responses after ~50 ms
                for (const auto& q : router->pendingDns_) {
                    ScheduledPacket sp;
                    sp.fromNode = routerId;
                    sp.toNode   = q.srcNodeId;
                    sp.sendAt   = simTime + 0.050; // 50 ms WAN delay

                    sp.pkt.id        = nextPacketId++;
                    sp.pkt.srcNodeId = routerId;
                    sp.pkt.dstNodeId = q.srcNodeId;
                    sp.pkt.sizeBytes = 120;
                    sp.pkt.createdAt = simTime;
                    sp.pkt.srcIp     = router->ip();
                    sp.pkt.dstIp     = q.srcIp;
                    sp.pkt.srcPort   = 53;
                    sp.pkt.dstPort   = q.srcPort;
                    sp.pkt.transport = TransportProtocol::UDP;
                    sp.pkt.app       = ApplicationProtocol::DNS;

                    scheduledPackets.push_back(sp);
                }
                router->pendingDns_.clear();

                // HTTPS responses after ~100 ms
                for (const auto& req : router->pendingHttps_) {
                    ScheduledPacket sp;
                    sp.fromNode = routerId;
                    sp.toNode   = req.srcNodeId;
                    sp.sendAt   = simTime + 0.100; // 100 ms WAN RTT

                    sp.pkt.id        = nextPacketId++;
                    sp.pkt.srcNodeId = routerId;
                    sp.pkt.dstNodeId = req.srcNodeId;
                    sp.pkt.sizeBytes = 50000; // pretend video or HTML chunk
                    sp.pkt.createdAt = simTime;
                    sp.pkt.srcIp     = "142.250.0.0"; // fake YouTube IP
                    sp.pkt.dstIp     = req.srcIp;
                    sp.pkt.srcPort   = 443;
                    sp.pkt.dstPort   = req.srcPort;
                    sp.pkt.transport = TransportProtocol::TCP;
                    sp.pkt.app       = ApplicationProtocol::HTTPS;

                    scheduledPackets.push_back(sp);
                }
                router->pendingHttps_.clear();
            }

            // inject scheduled WAN responses whose time has come
            for (std::size_t i = 0; i < scheduledPackets.size(); ) {
                if (scheduledPackets[i].sendAt <= simTime) {
                    network.spawnPacketOnLink(
                        scheduledPackets[i].pkt,
                        scheduledPackets[i].fromNode,
                        scheduledPackets[i].toNode
                    );
                    scheduledPackets.erase(scheduledPackets.begin() + i);
                } else {
                    ++i;
                }
            }
        }

        window.clear(sf::Color(30, 30, 30));
        renderer.draw();
        window.display();
    }

    return 0;
}