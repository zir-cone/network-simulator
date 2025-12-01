#include <SFML/Graphics.hpp>
#include <memory>
#include <iostream>
#include <random>
#include <cctype>
#include <cstdio>
#include <map>

#include "sim/Network.hpp"
#include "sim/Simulation.hpp"
#include "gui/Renderer.hpp"
#include "sim/Device.hpp"

// device types

class HomeDevice : public Device
{
public:
    HomeDevice(int id, NetworkScope scope, std::string ip, std::string name)
        : Device(id, scope),
          ip_(std::move(ip)),
          name_(std::move(name))
    {
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

        char buf[18];
        std::snprintf(buf, sizeof(buf), "02:00:00:00:%02X:%02X",
                      (id >> 8) & 0xFF, id & 0xFF);
        mac_ = buf;
        publicIp_ = "203.0.113.5";
    }

    const std::string& ip()       const { return ip_; }
    const std::string& name()     const { return name_; }
    const std::string& type()     const { return type_; }
    const std::string& user()     const { return user_; }
    const std::string& mac()      const { return mac_; }
    const std::string& publicIp() const { return publicIp_; }

    void tick(double now) override { (void)now; }

    void onPacketReceived(const Packet& pkt) override { (void)pkt; }

    DeviceInfo info() const override
    {
        return DeviceInfo{
            name_, user_, type_, ip_, publicIp_, mac_
        };
    }

private:
    std::string ip_, name_, type_, user_, mac_, publicIp_;
};

class RouterDevice : public Device
{
public:
    RouterDevice(int id, NetworkScope scope, std::string ip)
        : Device(id, scope), ip_(std::move(ip)) {}

    const std::string& ip() const { return ip_; }

    void tick(double /*now*/) override {}

    void onPacketReceived(const Packet& pkt) override
    {
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
    double sendAt;
};

// UI panel structs

struct NodePanelState {
    bool visible      = false;
    int  nodeId       = -1;
    sf::Vector2f pos  = {20.f, 20.f};
    sf::Vector2f size = {260.f, 150.f};
    bool dragging     = false;
    sf::Vector2f dragOffset{};
};

struct LinkPanelState {
    bool visible      = false;
    int  linkId       = -1;
    sf::Vector2f pos  = {20.f, 200.f};
    sf::Vector2f size = {360.f, 220.f};
    bool dragging     = false;
    sf::Vector2f dragOffset{};

    // camera
    float zoom        = 1.0f;
    sf::Vector2f offset{0.f, 0.f};
    bool panning      = false;
    sf::Vector2f panStart{};
};

// main

int main()
{
    const unsigned WIDTH  = 1280;
    const unsigned HEIGHT = 720;

    sf::RenderWindow window(
        sf::VideoMode(WIDTH, HEIGHT),
        "40NetworkSimulator v1.2"
    );
    window.setFramerateLimit(60);

    Network network;
    std::vector<ScheduledPacket> scheduledPackets;
    int nextId = 0;

    int routerId = network.addDevice(
        std::make_unique<RouterDevice>(nextId++, NetworkScope::Local, "192.168.0.1")
    );

    auto addHome = [&](const std::string& ip, const std::string& name) {
        int id = network.addDevice(
            std::make_unique<HomeDevice>(nextId++, NetworkScope::Local, ip, name)
        );
        double bw   = 100.0;
        double lat  = 5.0;
        if (name.find("TV") != std::string::npos ||
            name.find("Desktop") != std::string::npos ||
            name.find("television") != std::string::npos) {
            bw  = 1000.0;
            lat = 1.0;
        }
        network.addLink(routerId, id, bw, lat);
        return id;
    };

    int familyPcId     = addHome("192.168.0.10", "family-desktop");
    int laptopId       = addHome("192.168.0.11", "personal-laptop");
    int phoneId        = addHome("192.168.0.12", "johns-phone");
    int tabletId       = addHome("192.168.0.13", "family-tablet"); (void)tabletId;
    int tvId           = addHome("192.168.0.14", "family-television");
    int smartFridgeId  = addHome("192.168.0.20", "smart-fridge");

    Simulation sim(network);
    Renderer   renderer(window, network);

    bool paused    = false;
    sf::Clock clock;
    double timeScale = 1.0;

    std::cout << "Controls:\n"
              << "  Space: pause/resume\n"
              << "  Up/Down: time scale x10 / /10\n"
              << "  Left click node: open draggable node menu\n"
              << "  Left click link: open draggable, zoomable link view\n"
              << "  In link view: mouse wheel = zoom, middle-drag = pan\n"
              << "  Esc: quit\n";

    double simTime = 0.0;
    double nextDnsQueryTime      = 0.0;
    double nextWebBurstTime      = 0.0;
    double nextVideoChunkTime    = 0.0;
    double nextFridgePingTime    = 0.0;

    std::uint64_t nextPacketId = 1;

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> webClientDist(0, 2);

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

    // UI state
    NodePanelState nodePanel;
    LinkPanelState linkPanel;

    sf::Font uiFont;
    bool fontLoaded = uiFont.loadFromFile("resources/arial.ttf");

    // main loop 
    while (window.isOpen()) {
        sf::Event event{};
        while (window.pollEvent(event)) {
            switch (event.type) {
            case sf::Event::Closed:
                window.close();
                break;

            case sf::Event::KeyPressed:
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
                break;

            case sf::Event::MouseButtonPressed: {
                sf::Vector2f m(
                    static_cast<float>(event.mouseButton.x),
                    static_cast<float>(event.mouseButton.y));

                if (event.mouseButton.button == sf::Mouse::Left) {
                    // if clicking on node panel header, start dragging
                    if (nodePanel.visible) {
                        sf::FloatRect header(
                            nodePanel.pos.x,
                            nodePanel.pos.y,
                            nodePanel.size.x,
                            20.f
                        );
                        if (header.contains(m)) {
                            nodePanel.dragging = true;
                            nodePanel.dragOffset = m - nodePanel.pos;
                            break;
                        }
                    }

                    // if clicking on link panel header, start dragging
                    if (linkPanel.visible) {
                        sf::FloatRect header(
                            linkPanel.pos.x,
                            linkPanel.pos.y,
                            linkPanel.size.x,
                            20.f
                        );
                        if (header.contains(m)) {
                            linkPanel.dragging = true;
                            linkPanel.dragOffset = m - linkPanel.pos;
                            break;
                        }
                    }

                    // otherwise pick node / link in main scene
                    sf::Vector2f worldPos = window.mapPixelToCoords(
                        { event.mouseButton.x, event.mouseButton.y });

                    int nid = renderer.pickNode(worldPos);
                    if (nid != -1) {
                        nodePanel.visible = true;
                        nodePanel.nodeId  = nid;
                        // don't move if already visible; feels nicer
                        linkPanel.visible = false;
                        break;
                    }

                    int lid = renderer.pickLink(worldPos);
                    if (lid != -1) {
                        linkPanel.visible = true;
                        linkPanel.linkId  = lid;
                        // reset camera a bit
                        linkPanel.zoom    = 1.0f;
                        linkPanel.offset  = {0.f, 0.f};
                        nodePanel.visible = false;
                    }
                }
                else if (event.mouseButton.button == sf::Mouse::Middle) {
                    // start panning inside link view if inside its body
                    if (linkPanel.visible) {
                        sf::FloatRect body(
                            linkPanel.pos.x + 10.f,
                            linkPanel.pos.y + 30.f,
                            linkPanel.size.x - 20.f,
                            linkPanel.size.y - 40.f
                        );
                        if (body.contains(m)) {
                            linkPanel.panning  = true;
                            linkPanel.panStart = m;
                        }
                    }
                }
                break;
            }

            case sf::Event::MouseButtonReleased:
                if (event.mouseButton.button == sf::Mouse::Left) {
                    nodePanel.dragging = false;
                    linkPanel.dragging = false;
                }
                if (event.mouseButton.button == sf::Mouse::Middle) {
                    linkPanel.panning = false;
                }
                break;

            case sf::Event::MouseMoved: {
                sf::Vector2f m(
                    static_cast<float>(event.mouseMove.x),
                    static_cast<float>(event.mouseMove.y));
                if (nodePanel.dragging) {
                    nodePanel.pos = m - nodePanel.dragOffset;
                }
                if (linkPanel.dragging) {
                    linkPanel.pos = m - linkPanel.dragOffset;
                }
                if (linkPanel.panning) {
                    sf::Vector2f delta = m - linkPanel.panStart;
                    linkPanel.panStart = m;
                    linkPanel.offset += delta; // simple pixel offset
                }
                break;
            }

            case sf::Event::MouseWheelScrolled: {
                sf::Vector2f m(
                    static_cast<float>(event.mouseWheelScroll.x),
                    static_cast<float>(event.mouseWheelScroll.y));

                // zoom only when wheel is over link panel body
                if (linkPanel.visible) {
                    sf::FloatRect body(
                        linkPanel.pos.x + 10.f,
                        linkPanel.pos.y + 30.f,
                        linkPanel.size.x - 20.f,
                        linkPanel.size.y - 40.f
                    );
                    if (body.contains(m)) {
                        if (event.mouseWheelScroll.delta > 0.f)
                            linkPanel.zoom *= 1.2f;
                        else
                            linkPanel.zoom /= 1.2f;
                        if (linkPanel.zoom < 0.25f) linkPanel.zoom = 0.25f;
                        if (linkPanel.zoom > 5.f)   linkPanel.zoom = 5.f;
                    }
                }
                break;
            }

            default:
                break;
            }
        }

        double dtReal = clock.restart().asSeconds();
        if (dtReal > 0.1) dtReal = 0.1;
        double dtSim = dtReal * timeScale;

        if (!paused) {
            sim.step(dtSim);
            simTime += dtSim;

            // traffic generation
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

            if (simTime >= nextVideoChunkTime) {
                sendLanPacket(tvId, routerId,
                              deviceIp(tvId), deviceIp(routerId),
                              60000, 443,
                              TransportProtocol::TCP,
                              ApplicationProtocol::HTTPS, 4000);
                nextVideoChunkTime = simTime + 0.4;
            }

            if (simTime >= nextFridgePingTime) {
                sendLanPacket(smartFridgeId, routerId,
                              deviceIp(smartFridgeId), deviceIp(routerId),
                              55000, 443,
                              TransportProtocol::TCP,
                              ApplicationProtocol::HTTPS, 200);
                nextFridgePingTime = simTime + 10.0;
            }

            auto* router = dynamic_cast<RouterDevice*>(network.getDevice(routerId));
            if (router) {
                for (const auto& q : router->pendingDns_) {
                    ScheduledPacket sp;
                    sp.fromNode = routerId;
                    sp.toNode   = q.srcNodeId;
                    sp.sendAt   = simTime + 0.050;

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

                for (const auto& req : router->pendingHttps_) {
                    ScheduledPacket sp;
                    sp.fromNode = routerId;
                    sp.toNode   = req.srcNodeId;
                    sp.sendAt   = simTime + 0.100;

                    sp.pkt.id        = nextPacketId++;
                    sp.pkt.srcNodeId = routerId;
                    sp.pkt.dstNodeId = req.srcNodeId;
                    sp.pkt.sizeBytes = 50000;
                    sp.pkt.createdAt = simTime;
                    sp.pkt.srcIp     = "142.250.0.0";
                    sp.pkt.dstIp     = req.srcIp;
                    sp.pkt.srcPort   = 443;
                    sp.pkt.dstPort   = req.srcPort;
                    sp.pkt.transport = TransportProtocol::TCP;
                    sp.pkt.app       = ApplicationProtocol::HTTPS;

                    scheduledPackets.push_back(sp);
                }
                router->pendingHttps_.clear();
            }

            for (std::size_t i = 0; i < scheduledPackets.size();) {
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

        // draw 
        window.clear(sf::Color(30, 30, 30));
        renderer.draw();

        if (fontLoaded && nodePanel.visible && nodePanel.nodeId != -1) {
            auto* dev = network.getDevice(nodePanel.nodeId);
            if (dev) {
                DeviceInfo info = dev->info();

                sf::RectangleShape panel;
                panel.setSize(nodePanel.size);
                panel.setPosition(nodePanel.pos);
                panel.setFillColor(sf::Color(0, 0, 0, 200));
                panel.setOutlineThickness(1.f);
                panel.setOutlineColor(sf::Color::White);
                window.draw(panel);

                // header
                sf::RectangleShape header;
                header.setSize({nodePanel.size.x, 20.f});
                header.setPosition(nodePanel.pos);
                header.setFillColor(sf::Color(40, 40, 80, 220));
                window.draw(header);

                sf::Text title;
                title.setFont(uiFont);
                title.setCharacterSize(14);
                title.setFillColor(sf::Color::White);
                title.setString("Device Details");
                title.setPosition(nodePanel.pos.x + 6.f, nodePanel.pos.y + 2.f);
                window.draw(title);

                auto drawLine = [&](const std::string& s, float y) {
                    sf::Text t;
                    t.setFont(uiFont);
                    t.setCharacterSize(14);
                    t.setFillColor(sf::Color::White);
                    t.setString(s);
                    t.setPosition(nodePanel.pos.x + 10.f, y);
                    window.draw(t);
                };

                float base = nodePanel.pos.y + 28.f;
                drawLine("Name: "      + info.name,      base);
                drawLine("User: "      + info.user,      base + 18.f);
                drawLine("Type: "      + info.type,      base + 36.f);
                drawLine("Local IP: "  + info.localIp,   base + 54.f);
                drawLine("Public IP: " + info.publicIp,  base + 72.f);
                drawLine("MAC: "       + info.mac,       base + 90.f);
            }
        }

        // link panel with zoomable port view
        if (fontLoaded && linkPanel.visible && linkPanel.linkId != -1) {
            const Link* selLink = nullptr;
            for (const auto& l : network.links()) {
                if (l.id == linkPanel.linkId) { selLink = &l; break; }
            }
            if (selLink) {
                sf::RectangleShape panel;
                panel.setSize(linkPanel.size);
                panel.setPosition(linkPanel.pos);
                panel.setFillColor(sf::Color(0, 0, 0, 200));
                panel.setOutlineThickness(1.f);
                panel.setOutlineColor(sf::Color::White);
                window.draw(panel);

                sf::RectangleShape header;
                header.setSize({linkPanel.size.x, 20.f});
                header.setPosition(linkPanel.pos);
                header.setFillColor(sf::Color(80, 40, 40, 220));
                window.draw(header);

                sf::Text title;
                title.setFont(uiFont);
                title.setCharacterSize(14);
                title.setFillColor(sf::Color::White);
                title.setString("Link View (id " + std::to_string(selLink->id) + ")");
                title.setPosition(linkPanel.pos.x + 6.f, linkPanel.pos.y + 2.f);
                window.draw(title);

                // inner drawing area
                sf::FloatRect body(
                    linkPanel.pos.x + 10.f,
                    linkPanel.pos.y + 30.f,
                    linkPanel.size.x - 20.f,
                    linkPanel.size.y - 40.f
                );

                // background
                sf::RectangleShape bodyRect;
                bodyRect.setPosition({body.left, body.top});
                bodyRect.setSize({body.width, body.height});
                bodyRect.setFillColor(sf::Color(20, 20, 20, 230));
                window.draw(bodyRect);

                // build port lanes from in-flight packets
                struct Lane {
                    int port;
                };
                std::map<int, int> portToLane;
                int nextLane = 0;
                for (const auto& f : network.inFlightPackets()) {
                    if (f.linkId != selLink->id) continue;
                    int port = static_cast<int>(f.pkt.dstPort);
                    if (!portToLane.count(port)) {
                        portToLane[port] = nextLane++;
                    }
                }

                // draw lanes + packets
                float laneHeight = 28.f;
                float maxHeight = laneHeight * std::max(1, nextLane);
                float linkLenWorld = 1.0f; // t in [0,1]

                for (const auto& [port, laneIndex] : portToLane) {
                    float laneY = body.top + body.height / 2.f +
                                  (laneIndex - (nextLane - 1) / 2.f) * laneHeight
                                  + linkPanel.offset.y;

                    // lane line
                    sf::Vertex laneLine[] = {
                        sf::Vertex({body.left + 10.f + linkPanel.offset.x,
                                    laneY},
                                   sf::Color(120, 120, 120)),
                        sf::Vertex({body.left + body.width - 10.f + linkPanel.offset.x,
                                    laneY},
                                   sf::Color(120, 120, 120))
                    };
                    window.draw(laneLine, 2, sf::Lines);

                    // label
                    sf::Text lab;
                    lab.setFont(uiFont);
                    lab.setCharacterSize(12);
                    lab.setFillColor(sf::Color(200, 200, 200));
                    lab.setString("Port " + std::to_string(port));
                    lab.setPosition(body.left + 14.f + linkPanel.offset.x, laneY - 16.f);
                    window.draw(lab);
                }

                // draw packets as moving dots on their port lane
                for (const auto& f : network.inFlightPackets()) {
                    if (f.linkId != selLink->id) continue;
                    int port = static_cast<int>(f.pkt.dstPort);
                    int laneIndex = 0;
                    if (portToLane.count(port)) laneIndex = portToLane[port];

                    float laneY = body.top + body.height / 2.f +
                                  (laneIndex - (nextLane - 1) / 2.f) * laneHeight
                                  + linkPanel.offset.y;

                    // x along lane: f.t in [0,1], scaled by zoom
                    float x0 = body.left + 10.f;
                    float x1 = body.left + body.width - 10.f;
                    float laneWidth = (x1 - x0) * linkPanel.zoom;

                    float x = x0 + linkPanel.offset.x +
                              static_cast<float>(f.t) * laneWidth;

                    sf::CircleShape dot(4.f);
                    dot.setOrigin(4.f, 4.f);
                    dot.setPosition({x, laneY});

                    if (f.pkt.dstPort == 443)
                        dot.setFillColor(sf::Color(255, 80, 80));
                    else if (f.pkt.dstPort == 53)
                        dot.setFillColor(sf::Color(80, 200, 255));
                    else
                        dot.setFillColor(sf::Color(230, 230, 230));

                    window.draw(dot);
                }
            }
        }
        window.display();
    }

    return 0;
}