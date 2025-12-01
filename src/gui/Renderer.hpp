#pragma once
#include <SFML/Graphics.hpp>
#include "../sim/Network.hpp"
#include <vector>

struct NodeVisual 
{
    int deviceId;
    sf::Vector2f position;
};

class Renderer 
{
public:
    Renderer(sf::RenderWindow& window, Network& network);

    void updateLayout();
    void draw();

    int pickNode(const sf::Vector2f& point) const;
    int pickLink(const sf::Vector2f& point) const;
    const std::vector<NodeVisual>& visuals() const {return visuals_; }
private:
    const NodeVisual* findNodeVisual(int deviceId) const;

    sf::RenderWindow&       window_;
    Network&                network_;
    std::vector<NodeVisual> visuals_;
};
