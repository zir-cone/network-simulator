#include "Renderer.hpp"
#include <cmath>

Renderer::Renderer(sf::RenderWindow& window, Network& network)
    : window_(window), network_(network) {
    updateLayout();
}

const NodeVisual* Renderer::findNodeVisual(int deviceId) const
{
    for (const auto& v : visuals_) {
        if (v.deviceId == deviceId) return &v;
    }
    return nullptr;
}

void Renderer::updateLayout() 
{
    visuals_.clear();

    const float radius = 220.0f;
    const sf::Vector2f center(
        window_.getSize().x / 2.f,
        window_.getSize().y / 2.f
    );

    const auto& devices = network_.devices();
    const std::size_t n = devices.size();
    if (n == 0) return;

    for (std::size_t i = 0; i < n; ++i) {
        float angle =
            static_cast<float>(i) / static_cast<float>(n) * 2.f * 3.14159265f;

        sf::Vector2f pos = {
            center.x + radius * std::cos(angle),
            center.y + radius * std::sin(angle)
        };

        visuals_.push_back(NodeVisual{ devices[i]->id(), pos });
    }
}

void Renderer::draw() 
{
    // draw links
    for (const auto& link : network_.links()) {
        const NodeVisual* a = findNodeVisual(link.nodeA);
        const NodeVisual* b = findNodeVisual(link.nodeB);
        if (!a || !b) continue;

        sf::Vertex line[] = {
            sf::Vertex(a->position),
            sf::Vertex(b->position)
        };
        window_.draw(line, 2, sf::Lines);
    }
    // draw packets on links
    for (const auto& f : network_.inFlightPackets()) {
        const NodeVisual* from = findNodeVisual(f.fromNode);
        const NodeVisual* to   = findNodeVisual(f.toNode);
        if (!from || !to) continue;

        sf::Vector2f pos = (1.f - static_cast<float>(f.t)) * from->position
                           + static_cast<float>(f.t) * to->position;

        sf::CircleShape p(4.f);
        p.setOrigin(4.f, 4.f);
        p.setPosition(pos);

        // color by direction or port (simple scheme)
        if (f.pkt.dstPort == 443) {
            // HTTPS
            p.setFillColor(sf::Color(255, 80, 80)); // reddish
        } else if (f.pkt.dstPort == 53) {
            // DNS
            p.setFillColor(sf::Color(80, 200, 255)); // cyan-ish
        } else {
            p.setFillColor(sf::Color(200, 200, 200));
        }

        window_.draw(p);
    }
    // draw nodes on top
    for (const auto& v : visuals_) {
        sf::CircleShape circle(14.f);
        circle.setOrigin(14.f, 14.f);
        circle.setPosition(v.position);
        circle.setOutlineThickness(2.f);
        circle.setOutlineColor(sf::Color::White);

        auto dev = network_.getDevice(v.deviceId);
        if (!dev) continue;

        switch (dev->scope()) {
        case NetworkScope::Local:
            circle.setFillColor(sf::Color(100, 200, 100));   // green-ish
            break;
        case NetworkScope::Enterprise:
            circle.setFillColor(sf::Color(100, 150, 250));   // blue-ish
            break;
        case NetworkScope::Global:
            circle.setFillColor(sf::Color(250, 150, 100));   // orange-ish
            break;
        }
        window_.draw(circle);
    }
}