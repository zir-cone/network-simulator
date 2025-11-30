#include <SFML/Graphics.hpp>
#include <memory>
#include <iostream>
#include <random>

#include "sim/Network.hpp"
#include "sim/Simulation.hpp"
#include "gui/Renderer.hpp"
#include "sim/Device.hpp"

// simple home device class
class HomeDevice : public Device {
public:
    HomeDevice(int id, NetworkScope scope, std::string ip, std::string name)
        : Device(id, scope), ip_(std::move(ip)), name_(std::move(name)) {}

    const std::string& ip() const { return ip_; }
    const std::string& name() const { return name_; }

    void tick(double now) override {
        // no internal behavior atm; traffic is driven in main via timers.
        (void)now;
    }

    void onPacketReceived(const Packet& pkt) override {
        // debugging:
        // std::cout << "[" << name_ << "] got packet from node "
        //           << pkt.srcNodeId << " dstPort=" << pkt.dstPort << "\n";
        (void)pkt;
    }

private:
    std::string ip_;
    std::string name_;
};

class RouterDevice : public Device {
public:
    RouterDevice(int id, NetworkScope scope, std::string ip)
        : Device(id, scope), ip_(std::move(ip)) {}

    const std::string& ip() const { return ip_; }

    void tick(double now) override { (void)now; }

    void onPacketReceived(const Packet& pkt) override {
        // from the LAN's POV, router mainly forwards out or back in.
        // internet responses are faked in main().
        (void)pkt;
    }

private:
    std::string ip_;
};

int main() {
    const unsigned WIDTH  = 1280;
    const unsigned HEIGHT = 720;

    sf::RenderWindow window(
        sf::VideoMode(WIDTH, HEIGHT),
        "40NetworkSimulator v1.1"
    );
    window.setFramerateLimit(60);

    Network network;

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
        // connect via wireless fidelity or ethernet; here just randomize the latency a bit
        // tvs and desktops --> faster / lower latency to simulate ethernet
        double bw   = 100.0; // Mbps
        double lat  = 5.0;   // ms
        if (name.find("TV") != std::string::npos ||
            name.find("Desktop") != std::string::npos) {
            bw  = 1000.0;
            lat = 1.0;
        }
        network.addLink(routerId, id, bw, lat);
        return id;
    };

    // household devices
    int familyPcId     = addHome("192.168.0.10", "family-desktop");
    int laptopId       = addHome("192.168.0.11", "personal-laptop");
    int phoneId        = addHome("192.168.0.12", "johns-phone");
    int tabletId       = addHome("192.168.0.13", "family-tablet");
    int tvId           = addHome("192.168.0.14", "family-television");
    int smartFridgeId  = addHome("192.168.0.20", "smart-fridge");

    Simulation sim(network);
    Renderer   renderer(window, network);

    bool paused = false;
    sf::Clock clock;

    std::cout << "Controls:\n"
              << "  Space: pause/resume\n"
              << "  Esc:   quit\n";

    // traffic gen timers
    double simTime = 0.0;
    double nextDnsQueryTime      = 0.0;
    double nextWebBurstTime      = 0.0;
    double nextVideoChunkTime    = 0.0;
    double nextFridgePingTime    = 0.0;

    std::uint64_t nextPacketId = 1;

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> webClientDist(0, 2); // pick among pc, laptop, phone

    auto sendLanPacket = [&](int srcId, int dstId,
                             const std::string& srcIp,
                             const std::string& dstIp,
                             std::uint16_t srcPort,
                             std::uint16_t dstPort,
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
        p.transport = TransportProtocol::TCP;
        p.app       = appProto;

        network.spawnPacketOnLink(p, srcId, dstId);
    };

    auto deviceIp = [&](int id) -> std::string {
        auto* dev = dynamic_cast<HomeDevice*>(network.getDevice(id));
        if (!dev) {
            auto* r = dynamic_cast<RouterDevice*>(network.getDevice(id));
            if (r) return r->ip();
            return "0.0.0.0";
        }
        return dev->ip();
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
                }
            }
        }

        double dt = clock.restart().asSeconds();
        if (dt > 0.1) dt = 0.1;

        if (!paused) {
            sim.step(dt);
            simTime += dt;

            // dns queries every few seconds from a random client 
            if (simTime >= nextDnsQueryTime) {
                int clientIdx = webClientDist(rng);
                int clientId = (clientIdx == 0 ? familyPcId
                               : clientIdx == 1 ? laptopId
                                                : phoneId);
                sendLanPacket(clientId, routerId,
                              deviceIp(clientId), deviceIp(routerId),
                              40'000 + clientIdx, 53, // src ephemeral, dst=domain name system
                              ApplicationProtocol::DNS, 80);
                // response from router (spoofing internet)
                sendLanPacket(routerId, clientId,
                              deviceIp(routerId), deviceIp(clientId),
                              53, 40'000 + clientIdx,
                              ApplicationProtocol::DNS, 120);

                nextDnsQueryTime = simTime + 3.0; // every 3 seconds
            }

            // web browsing bursts (small https packets)
            if (simTime >= nextWebBurstTime) {
                int clientIdx = webClientDist(rng);
                int clientId = (clientIdx == 0 ? familyPcId
                               : clientIdx == 1 ? laptopId
                                                : phoneId);

                for (int i = 0; i < 5; ++i) {
                    sendLanPacket(clientId, routerId,
                                  deviceIp(clientId), "93.184.216.34", // example.com ipv4 addr
                                  50'000 + i, 443,
                                  ApplicationProtocol::HTTPS, 900);
                    // tiny https response packets coming back
                    sendLanPacket(routerId, clientId,
                                  "93.184.216.34", deviceIp(clientId),
                                  443, 50'000 + i,
                                  ApplicationProtocol::HTTPS, 1500);
                }

                nextWebBurstTime = simTime + 5.0; // roughly every 5 seconds
            }

            // continuous video chunks from televisions (big https packets) ---
            if (simTime >= nextVideoChunkTime) {
                // tv watching youtube
                sendLanPacket(tvId, routerId,
                              deviceIp(tvId), "142.250.0.0", // pretend youtube ipv4 addr
                              60'000, 443,
                              ApplicationProtocol::HTTPS, 4000); // request

                // chunk of video data back
                sendLanPacket(routerId, tvId,
                              "142.250.0.0", deviceIp(tvId),
                              443, 60'000,
                              ApplicationProtocol::HTTPS, 50'000); // bigger

                nextVideoChunkTime = simTime + 0.4; // 2.5 chunks/sec
            }

            // smart fridge occasional telemetry 
            if (simTime >= nextFridgePingTime) {
                sendLanPacket(smartFridgeId, routerId,
                              deviceIp(smartFridgeId), deviceIp(routerId),
                              55'000, 443,
                              ApplicationProtocol::HTTPS, 200);
                nextFridgePingTime = simTime + 10.0;
            }
        }

        window.clear(sf::Color(30, 30, 30));
        renderer.draw();
        window.display();
    }

    return 0;
}